#include "BlockIndexer.h"
#include "Logger.h"
#include "ycash_rpc_client.h"
#include "orm/PostgresCopy.h"
#include "orm/PostgresPool.h"
#include "ZeroMQListener.h"
#include <atomic>
#include <cstdint>
#include <libpq-fe.h>
#include <pthread.h>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <format>
#include <utility>

static Logger& logger = Logger::instance();

static std::string bytea_to_hex_string(const char* bytea_escaped) {
    if (!bytea_escaped || bytea_escaped[0] == '\0') {
        return "";  // NULL or empty
    }

    size_t len;
    uint8_t* raw_bytes = PQunescapeBytea(
        reinterpret_cast<const unsigned char*>(bytea_escaped), &len);

    if (!raw_bytes || len == 0) {
        return "";  // Error or empty
    }

    // Convert to hex string using std::format for each byte
    std::string hex_string;
    hex_string.reserve(len * 2);

    for (size_t i = 0; i < len; ++i) {
        hex_string += std::format("{:02x}", raw_bytes[i]);
    }

    PQfreemem(raw_bytes);
    return hex_string;
}

std::string batch_type_string(batch_type bt) {
    switch (bt) {
        case BT_HEADERS: return "headers";
        default: return "unknown";
    }
}

BlockIndexer::BlockIndexer() : db_connection_(nullptr) {
}

BlockIndexer::~BlockIndexer() {
    stop();
    if (db_connection_) {
        PQfinish(static_cast<PGconn*>(db_connection_));
        db_connection_ = nullptr;
    }
}

BlockIndexer& BlockIndexer::instance() {
    static BlockIndexer instance;
    return instance;
}

bool BlockIndexer::initialize(const RpcConfig::Config& config) {
    rpc_config = config;

    // Set queue depth from config (backpressure control)
    max_queue_depth = config.queue_depth;
    logger.infof("BlockIndexer: Queue depth set to {} batches", max_queue_depth);

    // Initialize database connection
    if (!initialize_database()) {
        logger.error("BlockIndexer: Failed to initialize database connection");
        return false;
    }

    // Get RPC client singleton instance (already initialized in main())
    rpc_client = &YcashRpcClient::instance();

    logger.info("BlockIndexer: Initialized successfully");
    return true;
}

bool BlockIndexer::initialize_database() {
    // Build PostgreSQL connection string, with TCP keepalives and connect_timeout
    // so a silently-broken socket surfaces in minutes rather than hours.
    std::string conn_str = augment_conn_str_with_keepalives(
        "host=" + rpc_config.postgresql_host +
        " port=" + std::to_string(rpc_config.postgresql_port) +
        " dbname=" + rpc_config.postgresql_database +
        " user=" + rpc_config.postgresql_username +
        " password=" + rpc_config.postgresql_password);

    PGconn* conn = PQconnectdb(conn_str.c_str());

    if (PQstatus(conn) != CONNECTION_OK) {
        logger.errorf("BlockIndexer DB: Connection failed: {}", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }

    harden_pg_connection(conn);
    db_connection_ = conn;
    logger.info("BlockIndexer DB: Connected successfully");

    return true;
}

void BlockIndexer::start() {
    if (flag_running.exchange(true)) {
        return;  // Already running
    }

    // Perform startup check to see if catchup is needed
    if (perform_startup_check()) {
        logger.info("BlockIndexer: Missing blocks detected - starting catchup indexing");
    } else {
        logger.info("BlockIndexer: Database is synced with blockchain, monitoring for new blocks");
    }

    indexing_thread = std::jthread(&BlockIndexer::thread_func_indexer, this);
    logger.info("BlockIndexer: Started background indexing thread");
}

bool BlockIndexer::interruptible_sleep(std::stop_token stop_token, int seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (stop_token.stop_requested()) {
            return false;  // Interrupted by stop signal
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;  // Sleep completed naturally
}

bool BlockIndexer::wait_for_new_blocks_or_timeout(std::stop_token stop_token, int timeout_seconds) {
    // Wait for ZeroMQ block events with a timeout fallback
    // Uses condition variable for immediate notification on new blocks
    // Supports interruptible shutdown via stop_token

    // Check stop signal before waiting
    if (stop_token.stop_requested()) {
        return false;
    }

    // Wait for block event (with timeout as fallback, interruptible by stop_token)
    bool event_received = ZeroMQListener::instance().wait_for_block_event(stop_token, timeout_seconds);

    if (stop_token.stop_requested()) {
        return false;  // Stop signal received
    }

    if (event_received) {
        logger.debug("BlockIndexer: Received block event, waking up from sync wait");
    } else {
        logger.debugf("BlockIndexer: Timeout waiting for blocks ({}s), checking blockchain height", timeout_seconds);
    }

    return true;  // Return to re-check blockchain height (either event or timeout)
}


void BlockIndexer::stop() {
    if (!flag_running) {
        return;
    }

    logger.info("BlockIndexer: Stop requested, signaling threads...");

    // Request stop on all threads (non-blocking, uses stop_token)
    indexing_thread.request_stop();
    rpc_producer_thread.request_stop();
    db_consumer_thread.request_stop();
    // Wake up threads waiting on condition variables
    queue_cv.notify_all();
    backpressure_cv.notify_all();

    indexing_thread.join();

    flag_running = false;
    logger.info("BlockIndexer: Stopped successfully");
}

void BlockIndexer::thread_func_indexer(std::stop_token stop_token)
{
    pthread_setname_np(pthread_self(), "Inzyght-indexer");
    logger.info("BlockIndexer: Indexing thread started");

    while (!stop_token.stop_requested())
    {
        try
        {
            // Let's decide what type of indexing we should prioritize
            // Get current blockchain height
            int blockchain_height = get_blockchain_height();
            if (blockchain_height <= 0)
            {
                logger.warn("BlockIndexer: Could not determine blockchain height");
                if (!interruptible_sleep(stop_token, 30)) continue;
                continue;
            }

            int current_height = get_last_indexed_height();
            if (current_height < 0)
            {
                // Couldn't read the indexed height (DB connection down or query
                // failed). Do NOT treat this as genesis - that would re-index
                // from height 0 into a populated DB and cause PK violations.
                // Skip this cycle and retry once the DB is reachable again.
                logger.warn("BlockIndexer: Could not read last indexed height (DB unavailable); skipping cycle");
                if (!interruptible_sleep(stop_token, 30)) continue;
                continue;
            }

            progress_total_height = blockchain_height;
            progress_current_height = current_height;

            // Periodically refresh transaction count cache from database (for real-time progress display)
            // This ensures the frontend sees progress even while large batches are being processed
            refresh_transaction_count_cache();

            // Check for reorg before proceeding. We only need to look when our
            // tip is within the daemon's reorg horizon — anything deeper is
            // locked in. Inclusive boundary so a tip exactly MAX_REORG_DEPTH
            // behind is still checked.
            if (current_height >= blockchain_height - MAX_REORG_DEPTH && detect_reorg(current_height))
            {
                logger.warn("BlockIndexer: Reorg detected - finding common ancestor");
                int common_ancestor = find_common_ancestor(current_height - 1);

                if (common_ancestor < 0)
                {
                    update_sync_progress(BT_HEADERS, current_height, blockchain_height, "error",
                                         "No common ancestor within MAX_REORG_DEPTH - manual intervention required");
                    if (!interruptible_sleep(stop_token, 60)) continue;
                    continue;
                }

                if (rollback_to_height(common_ancestor))
                {
                    current_height = common_ancestor;
                    // sync_progress was already reset inside rollback_to_height's transaction.
                    // record_reorg_event is audit-only; failure to log it does not affect correctness.
                    record_reorg_event(current_height, last_reorg_depth.load(std::memory_order_acquire));
                    logger.infof("BlockIndexer: Resuming indexing after reorg at height {}", current_height);
                }
                else
                {
                    update_sync_progress(BT_HEADERS, current_height, blockchain_height, "error", "Reorg rollback failed");
                    if (!interruptible_sleep(stop_token, 30)) continue;
                    continue;
                }
            }

            if (current_height < blockchain_height)
            {
                // Reset the sync-session start whenever we transition from
                // "idle at the tip" back into active syncing, so the average
                // reflects *this* catch-up session rather than the lifetime
                // average since process start.
                const bool was_syncing = flag_syncing.exchange(true);
                if (!was_syncing || progress_sync_start_time == 0)
                {
                    progress_sync_start_time = std::time(nullptr);
                    progress_sync_start_height = current_height;

                    // Reset trailing-window speed samples so the new session
                    // starts with a clean window instead of inheriting stale
                    // samples from the previous catch-up.
                    {
                        std::scoped_lock<std::mutex> lock(speed_ring_mutex);
                        speed_ring_head = 0;
                        speed_ring_count = 0;
                    }

                    // Initialize rolling window for recent speed
                    progress_recent_window_start_height = current_height;
                    progress_recent_window_start_time = std::time(nullptr);

                    logger.infof("BlockIndexer: Starting headers sync from height {} to {}", current_height, blockchain_height);
                }

                // Index next batch of headers
                if (current_height == 0) current_height = -1;
                int batch_end = std::min(current_height + HEADERS_BATCH_SIZE, blockchain_height);
                logger.infof("BlockIndexer: Indexing block headers {} to {}", current_height + 1, batch_end);
                

                if (index_chain_data_batch(stop_token, BT_HEADERS, current_height + 1, batch_end))
                {
                    // The consumer already advanced sync_progress atomically with
                    // each committed sub-batch; refresh status/total at the height
                    // actually committed (== batch_end on full success).
                    update_sync_progress(BT_HEADERS, progress_current_height.load(std::memory_order_acquire), blockchain_height, "syncing");
                }
                else
                {
                    // Partial failure: report the error at the last committed
                    // height, NOT current_height — moving the pointer back would
                    // re-COPY already-committed sub-batches and hit PK violations.
                    update_sync_progress(BT_HEADERS, progress_current_height.load(std::memory_order_acquire), blockchain_height, "error", "Failed to index block batch");
                    interruptible_sleep(stop_token, 10);
                }
            }
            else
            {
                // Blockchain is fully synced - wait for new blocks via ZeroMQ events
                flag_syncing = false;
                update_sync_progress(BT_HEADERS, current_height, blockchain_height, "completed");
                logger.infof("BlockIndexer: Block headers indexing completed at id {}", current_height);

                // Wait for new blocks with a 30-second timeout
                // This prevents busy-looping while allowing periodic checks even without ZeroMQ events
                logger.debug("BlockIndexer: Waiting for new blocks (via ZeroMQ or timeout)");
                if (!wait_for_new_blocks_or_timeout(stop_token, 30)) {
                    continue;  // Stop signal received
                }
                // Continue loop to re-check blockchain height
            }
        }
        catch (const std::exception& e)
        {
            logger.errorf("BlockIndexer: Exception in indexing thread: {}", e.what());
            interruptible_sleep(stop_token, 10);
        }
    }

    logger.info("BlockIndexer: Indexing thread exiting");
}

int BlockIndexer::get_blockchain_height()
{
    if (!rpc_client) {
        return -1;
    }

    try {
        BlockCountResponse response = rpc_client->get_block_count();
        return response.count;

    } catch (const std::exception& e) {
        logger.errorf("BlockIndexer: Failed to get block count: {}", e.what());
    }

    return -1;
}

int BlockIndexer::get_last_indexed_height()
{
    PGconn* conn = static_cast<PGconn*>(db_connection_);

    // Return -1 (not 0) on any error: a dead connection or failed query must NOT
    // be mistaken for "genesis / nothing indexed", which would trigger a
    // destructive re-sync from height 0 against an already-populated database.
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        return -1;
    }

    PGresult* res = PQexec(conn, "SELECT last_indexed_height FROM sync_progress WHERE name = 'headers'");

    int current_height = -1;
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
    {
        // 0 rows = headers row genuinely absent (truly empty DB) -> height 0.
        // >=1 row = use the stored height.
        current_height = (PQntuples(res) > 0) ? atoi(PQgetvalue(res, 0, 0)) : 0;
    }
    if (res) {
        PQclear(res);
    }

    return current_height;
}

int BlockIndexer::get_transactions_count() const
{
    PGconn* conn = static_cast<PGconn*>(db_connection_);

    // Check connection status
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        return 0;
    }

    PGresult* res = PQexec(conn, "SELECT total_height FROM sync_progress WHERE name = 'transactions'");

    int transactions_count = 0;
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        transactions_count = atoi(PQgetvalue(res, 0, 0));
    }
    if (res) {
        PQclear(res);
    }

    return transactions_count;
}


int BlockIndexer::get_indexed_transactions_count() const
{
    PGconn* conn = static_cast<PGconn*>(db_connection_);

    // Check connection status
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        return 0;
    }

    PGresult* res = PQexec(conn, "SELECT last_indexed_height FROM sync_progress WHERE name = 'transactions'");

    int indexed_transactions_count = 0;
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
    {
        indexed_transactions_count = atoi(PQgetvalue(res, 0, 0));
    }
    if (res) {
        PQclear(res);
    }

    return indexed_transactions_count;
}


bool BlockIndexer::index_chain_data_batch(std::stop_token stop_token, batch_type bt, int start, int end)
{
    if (!stop_token.stop_requested())
    {
        logger.infof("BlockIndexer: Starting parallel batch processing ({} from {} to {})", batch_type_string(bt), start, end);

        // Reset synchronization state
        flag_producer_stopped = false;
        flag_consumer_failed = false;
        pending_batch_count = 0;
        {
            std::scoped_lock<std::mutex> lock(queue_mutex);
            data_queue.clear();
            data_queue.shrink_to_fit();
        }

        // Start producer thread (fetches data from RPC)
        rpc_producer_thread = std::jthread(&BlockIndexer::thread_func_rpc_producer, this, bt, start, end);

        // Start consumer thread (processes blocks and inserts into DB)
        db_consumer_thread = std::jthread(&BlockIndexer::thread_func_db_consumer, this);

        // Wait for both threads to complete
        if (rpc_producer_thread.joinable()) {
            rpc_producer_thread.join();
        }
        if (db_consumer_thread.joinable()) {
            db_consumer_thread.join();
        }

        // Report the consumer's outcome to the caller so it never advances the
        // sync pointer past a batch that failed to fully commit.
        if (flag_consumer_failed.load(std::memory_order_acquire))
        {
            logger.warnf("BlockIndexer: Parallel {} batch processing ended with consumer failure", batch_type_string(bt));
            return false;
        }

        logger.infof("BlockIndexer: Parallel {} batch processing complete", batch_type_string(bt));
        return true;
    }

    logger.warnf("BlockIndexer: Not starting parallel batch processing, shutting down!");
    return false;
}


// Fetch address balances from Insight API in batches
// Address balance calculation methods removed - balances queried on-demand via Insight API getaddressbalance

void BlockIndexer::update_sync_progress(batch_type bt, int current_height, int total_height,
                                      std::string_view status, std::string_view error_msg) {
    // Note: No mutex needed - only called from indexing_thread_func() (single writer)
    // PostgreSQL's update_sync_progress() function provides atomic updates
    PGconn* conn = static_cast<PGconn*>(db_connection_);

    // Check connection status
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        logger.warn("BlockIndexer: Database connection is not available for sync progress update");
        return;
    }

    std::string query = std::format("SELECT update_sync_progress('{}', $1, $2, $3, $4, $5)", batch_type_string(bt));

    // Store parameter values in strings to keep them alive
    std::string current_height_str = std::to_string(current_height);
    std::string total_height_str = std::to_string(total_height);
    std::string indexed_count_str = std::to_string(current_height);
    std::string status_str = std::string(status);
    std::string error_msg_str = std::string(error_msg);

    const char* param_values[] = {
        current_height_str.c_str(),
        total_height_str.c_str(),
        indexed_count_str.c_str(),
        status_str.c_str(),
        error_msg_str.c_str()
    };

    int param_lengths[] = {
        (int)current_height_str.length(),
        (int)total_height_str.length(),
        (int)indexed_count_str.length(),
        (int)status_str.length(),
        (int)error_msg_str.length()
    };

    int param_formats[] = {0, 0, 0, 0, 0};

    PGresult* res = PQexecParams(conn, query.c_str(), 5, nullptr, param_values, param_lengths, param_formats, 0);

    if (res && PQresultStatus(res) != PGRES_TUPLES_OK) {
        logger.warnf("BlockIndexer: Failed to update sync progress: {}", PQerrorMessage(conn));
    }

    if (res) {
        PQclear(res);
    }
}

void BlockIndexer::refresh_transaction_count_cache() {
    // Only refresh if cache is stale (older than 3 seconds)
    int64_t now = std::time(nullptr);
    int64_t last_update = cached_tx_last_update_time;

    if (now - last_update < 3) {
        return;  // Cache is fresh enough
    }

    PGconn* conn = static_cast<PGconn*>(db_connection_);

    // Check connection status
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        return;  // Can't refresh, but don't crash
    }

    // Query both counts in a single query for efficiency
    PGresult* res = PQexec(conn,
        "SELECT "
        "  (SELECT COALESCE(last_indexed_height, 0) FROM sync_progress WHERE name = 'transactions') AS current, "
        "  (SELECT COALESCE(total_height, 0) FROM sync_progress WHERE name = 'transactions') AS total");

    if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        int current = atoi(PQgetvalue(res, 0, 0));
        int total = atoi(PQgetvalue(res, 0, 1));

        // Only update if values changed (reduce unnecessary updates)
        if (current != cached_tx_current_height.load() || total != cached_tx_total_height.load()) {
            cached_tx_current_height = current;
            cached_tx_total_height = total;
            cached_tx_last_update_time = now;
        }
    }

    if (res) {
        PQclear(res);
    }
}

double BlockIndexer::get_sync_progress() const {
    if (progress_total_height <= 0) {
        return 0.0;
    }

    double progress = (double)progress_current_height / (double)progress_total_height * 100.0;
    return progress > 100.0 ? 100.0 : progress;
}

int BlockIndexer::get_current_height() const {
    return progress_current_height;
}

double BlockIndexer::get_average_sync_speed() const {
    int64_t start_time    = progress_sync_start_time;
    int     start_height  = progress_sync_start_height;
    int     current_height = progress_current_height;

    if (start_time == 0 || current_height <= start_height) {
        return 0.0;
    }

    // While the indexer is idle at the tip (flag_syncing == false), wall-clock
    // keeps ticking but no new blocks arrive, which would silently decay the
    // reported average toward zero. Freeze the elapsed clock to the moment we
    // caught up — the average then reflects the actual catch-up rate, not
    // "blocks per second since process start including all idle time".
    int64_t end_time;
    if (flag_syncing.load(std::memory_order_acquire)) {
        end_time = std::time(nullptr);
    } else {
        // Use the time of the newest sample if we have one; otherwise now.
        std::scoped_lock<std::mutex> lock(speed_ring_mutex);
        if (speed_ring_count > 0) {
            size_t newest = (speed_ring_head + SPEED_RING_CAPACITY - 1) % SPEED_RING_CAPACITY;
            end_time = speed_ring[newest].time;
        } else {
            end_time = std::time(nullptr);
        }
    }

    int64_t elapsed_seconds = end_time - start_time;
    if (elapsed_seconds <= 0) {
        return 0.0;
    }

    int blocks_processed = current_height - start_height;
    return static_cast<double>(blocks_processed) / static_cast<double>(elapsed_seconds);
}

void BlockIndexer::record_speed_sample(int height) {
    std::scoped_lock<std::mutex> lock(speed_ring_mutex);
    speed_ring[speed_ring_head] = SpeedSample{std::time(nullptr), height};
    speed_ring_head = (speed_ring_head + 1) % SPEED_RING_CAPACITY;
    if (speed_ring_count < SPEED_RING_CAPACITY) ++speed_ring_count;
}

double BlockIndexer::get_current_batch_speed() const {
    // Trailing-window blocks/sec over the last SPEED_WINDOW_SECONDS, computed
    // from the (time, height) ring. Naturally settles to 0 when the indexer
    // is idle (no new samples, oldest sample stays put, but as soon as a new
    // sample lands the window collapses to the actual rate). Returns 0 with
    // fewer than 2 samples or zero window span — not a fallback to average,
    // because that would defeat the purpose during idle periods.
    std::scoped_lock<std::mutex> lock(speed_ring_mutex);
    if (speed_ring_count < 2) return 0.0;

    int64_t now_sec  = std::time(nullptr);
    int64_t cutoff  = now_sec - SPEED_WINDOW_SECONDS;

    // newest sample is at (head - 1); oldest sample within the window is the
    // first one we encounter walking backwards whose time >= cutoff, or the
    // oldest sample in the ring if all are within the window.
    size_t newest_idx = (speed_ring_head + SPEED_RING_CAPACITY - 1) % SPEED_RING_CAPACITY;
    SpeedSample newest = speed_ring[newest_idx];

    SpeedSample oldest = newest;
    for (size_t i = 1; i < speed_ring_count; ++i) {
        size_t idx = (speed_ring_head + SPEED_RING_CAPACITY - 1 - i) % SPEED_RING_CAPACITY;
        SpeedSample s = speed_ring[idx];
        if (s.time < cutoff) break;
        oldest = s;
    }

    int64_t dt = newest.time - oldest.time;
    int     dh = newest.height - oldest.height;
    if (dt <= 0 || dh <= 0) return 0.0;

    return static_cast<double>(dh) / static_cast<double>(dt);
}

int64_t BlockIndexer::get_estimated_time_remaining() const {
    int current_height = progress_current_height;
    int total_height = progress_total_height;
    int start_height = progress_sync_start_height;
    int64_t sync_start_time = progress_sync_start_time;

    // If sync is complete or not started
    if (current_height >= total_height || sync_start_time <= 0) {
        return 0;
    }

    // Use average speed to estimate total duration from start
    double avg_speed = get_average_sync_speed();
    if (avg_speed <= 0.0) {
        return 0;
    }

    // Calculate total duration estimate based on all blocks to process
    int total_blocks_to_process = total_height - start_height;
    int64_t total_estimated_seconds = (int64_t)((double)total_blocks_to_process / avg_speed);

    // Calculate elapsed time so far
    int64_t current_time = std::time(nullptr);
    int64_t elapsed_seconds = current_time - sync_start_time;

    // ETA = total duration - elapsed time
    int64_t eta_seconds = total_estimated_seconds - elapsed_seconds;

    // Return 0 if ETA is negative (shouldn't happen but safety check)
    return eta_seconds > 0 ? eta_seconds : 0;
}

bool BlockIndexer::perform_startup_check() {
    try {
        // Get current blockchain height from RPC
        int blockchain_height = get_blockchain_height();
        if (blockchain_height <= 0) {
            logger.warn("BlockIndexer: Could not determine blockchain height during startup check");
            return false;
        }

        // Query current indexed height from database
        // Use MAX(height) from blocks table instead of sync_progress to avoid duplicate key errors
        // This handles cases where indexer crashed after inserting blocks but before updating sync_progress
        PGconn* conn = static_cast<PGconn*>(db_connection_);
        PGresult* res = PQexec(conn,
            "SELECT COALESCE(MAX(height), 0) FROM blocks");

        int last_indexed_height = 0;
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            last_indexed_height = atoi(PQgetvalue(res, 0, 0));
        }
        PQclear(res);

        // Also check sync_progress for comparison
        res = PQexec(conn,
            "SELECT COALESCE(last_indexed_height, 0) FROM sync_progress WHERE name = 'headers'");
        int sync_progress_height = 0;
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            sync_progress_height = atoi(PQgetvalue(res, 0, 0));
        }
        PQclear(res);

        // Use the maximum of both to ensure we don't re-index existing blocks
        if (sync_progress_height < last_indexed_height) {
            logger.warnf("BlockIndexer: sync_progress ({}) is behind actual blocks table ({}), using actual height to avoid duplicates", sync_progress_height, last_indexed_height);
        }
        last_indexed_height = std::max(last_indexed_height, sync_progress_height);

        // Update progress with current blockchain height
        progress_total_height = blockchain_height;
        progress_current_height = last_indexed_height;

        int block_difference = blockchain_height - last_indexed_height;

        if (block_difference <= 0) {
            // Heights match, but we need to verify block hashes to detect reorg
            // If a reorg happened while we were down, heights could match but hashes differ
            if (last_indexed_height > 0) {
                std::string db_hash = get_block_hash_from_db(last_indexed_height);
                std::string rpc_hash = get_block_hash_from_rpc(last_indexed_height);

                if (!db_hash.empty() && !rpc_hash.empty() && db_hash != rpc_hash) {
                    logger.warn("BlockIndexer Startup Check: REORG DETECTED during shutdown!");
                    logger.warnf("  - Height: {} (same height, different hash)", last_indexed_height);
                    logger.warnf("  - DB hash:  {}", db_hash);
                    logger.warnf("  - RPC hash: {}", rpc_hash);
                    logger.info("  - Will rollback and re-index from common ancestor");
                    return true;  // Force reorg recovery on next iteration
                }
            }

            logger.infof("BlockIndexer: Database is synced - last_indexed={}, blockchain_tip={}",
                       last_indexed_height, blockchain_height);
            return false;  // No catchup needed
        }

        logger.info("BlockIndexer Startup Check:");
        logger.infof("  - Blockchain tip: {}", blockchain_height);
        logger.infof("  - Last indexed: {}", last_indexed_height);
        logger.infof("  - Missing blocks: {}", block_difference);
        logger.info("  - Catchup needed: YES - will start indexing immediately");

        // Initialize sync progress if it doesn't exist
        update_sync_progress(BT_HEADERS, last_indexed_height, blockchain_height, "syncing");

        return true;  // Catchup needed

    } catch (const std::exception& e) {
        logger.errorf("BlockIndexer: Exception during startup check: {}", e.what());
        return false;
    }
}

std::string BlockIndexer::get_block_hash_from_rpc(int height) {
    if (!rpc_client) {
        return "";
    }

    try {
        BlockHashResponse result = rpc_client->get_block_hash(height);
        return result.hash;
    } catch (const std::exception& e) {
        logger.debugf("BlockIndexer: Failed to get block hash from RPC at height {}: {}",
                    height, e.what());
    }

    return "";
}

std::string BlockIndexer::get_block_hash_from_db(int height) {
    PGconn* conn = static_cast<PGconn*>(db_connection_);
    if (!conn) {
        return "";
    }

    std::string height_str = std::to_string(height);
    const char* params[] = { height_str.c_str() };
    PGresult* res = PQexecParams(conn,
        "SELECT hash FROM blocks WHERE height = $1 LIMIT 1",
        1, nullptr, params, nullptr, nullptr, 0);

    std::string hash;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        hash = bytea_to_hex_string(PQgetvalue(res, 0, 0));
    }

    PQclear(res);
    return hash;
}

bool BlockIndexer::detect_reorg(int height_to_check) {
    if (height_to_check <= 0) {
        return false;
    }

    std::string rpc_hash = get_block_hash_from_rpc(height_to_check);
    std::string db_hash = get_block_hash_from_db(height_to_check);

    if (rpc_hash.empty() || db_hash.empty()) {
        return false;
    }

    if (rpc_hash != db_hash) {
        logger.warnf("REORG DETECTED at height {}", height_to_check);
        logger.warnf("  DB hash:  {}", db_hash);
        logger.warnf("  RPC hash: {}", rpc_hash);
        return true;
    }

    return false;
}

int BlockIndexer::find_common_ancestor(int start_height) {
    // Bound the scan: the daemon won't accept reorgs deeper than MAX_REORG_DEPTH,
    // so a divergence beyond that means something is wrong (corrupted DB, wrong
    // chain, etc.) — escalate rather than scan all the way to genesis.
    int min_height = std::max(0, start_height - MAX_REORG_DEPTH);
    for (int height = start_height; height >= min_height; height--) {
        std::string rpc_hash = get_block_hash_from_rpc(height);
        std::string db_hash = get_block_hash_from_db(height);

        if (!rpc_hash.empty() && !db_hash.empty() && rpc_hash == db_hash) {
            logger.infof("BlockIndexer: Found common ancestor at height {}", height);
            return height;
        }

        if (height % 10 == 0) {
            logger.debugf("BlockIndexer: Still scanning for common ancestor... at height {}", height);
        }
    }

    logger.errorf("BlockIndexer: FATAL - No common ancestor within {} blocks of height {} - chain divergence beyond daemon's reorg limit",
                  MAX_REORG_DEPTH, start_height);
    return -1;
}

bool BlockIndexer::rollback_to_height(int target_height) {
    PGconn* conn = static_cast<PGconn*>(db_connection_);
    if (!conn) {
        return false;
    }

    // Defensive depth bound: a single rollback should never delete more than
    // MAX_REORG_DEPTH blocks. find_common_ancestor already enforces this, so
    // anything larger here indicates a logic error upstream — refuse rather
    // than wipe a large prefix of the indexed chain.
    int current_tip = progress_current_height.load(std::memory_order_acquire);
    if (target_height < 0 || current_tip - target_height > MAX_REORG_DEPTH) {
        logger.errorf("BlockIndexer: Refusing rollback of depth {} (tip {} -> target {}); exceeds MAX_REORG_DEPTH={}",
                      current_tip - target_height, current_tip, target_height, MAX_REORG_DEPTH);
        return false;
    }

    try {
        std::scoped_lock<std::mutex> lock(reorg_mutex);

        // Start transaction
        PGresult* res = PQexec(conn, "BEGIN");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            logger.errorf("BlockIndexer: Failed to start transaction: {}", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        PQclear(res);

        // Count affected data
        int affected_block_count = 0;
        int affected_tx_count = 0;

        std::string target_str = std::to_string(target_height);
        const char* target_params[] = { target_str.c_str() };

        // Count blocks to delete
        res = PQexecParams(conn,
            "SELECT COUNT(*) FROM blocks WHERE height > $1",
            1, nullptr, target_params, nullptr, nullptr, 0);
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            affected_block_count = atoi(PQgetvalue(res, 0, 0));
        }
        PQclear(res);

        // Count transactions to delete
        res = PQexecParams(conn,
            "SELECT COUNT(*) FROM transactions WHERE block_height > $1",
            1, nullptr, target_params, nullptr, nullptr, 0);
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            affected_tx_count = atoi(PQgetvalue(res, 0, 0));
        }
        PQclear(res);

        // Delete in order of dependencies, then update sync_progress in the same
        // transaction so a crash here cannot leave sync_progress pointing past
        // blocks/transactions that no longer exist.
        const char* delete_queries[] = {
            "DELETE FROM transactions WHERE block_height > $1",
            "DELETE FROM blocks WHERE height > $1"
        };

        for (const char* query : delete_queries) {
            res = PQexecParams(conn, query, 1, nullptr, target_params, nullptr, nullptr, 0);
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                logger.errorf("BlockIndexer: Rollback query failed: {}", PQerrorMessage(conn));
                PQclear(res);
                PGresult* rollback_res = PQexec(conn, "ROLLBACK");
                if (rollback_res) {
                    PQclear(rollback_res);
                }
                return false;
            }
            PQclear(res);
        }

        // Reset sync_progress for headers to the rollback target inside the
        // same transaction. total_height is left equal to current — the next
        // indexer iteration will refresh it from RPC.
        {
            const char* progress_params[] = {
                target_str.c_str(),  // current
                target_str.c_str(),  // total
                target_str.c_str(),  // indexed_count
                "syncing",          // status
                ""                  // error_message
            };
            res = PQexecParams(conn,
                "SELECT update_sync_progress('headers', $1, $2, $3, $4, $5)",
                5, nullptr, progress_params, nullptr, nullptr, 0);
            if (PQresultStatus(res) != PGRES_TUPLES_OK && PQresultStatus(res) != PGRES_COMMAND_OK) {
                logger.errorf("BlockIndexer: Failed to reset sync_progress in rollback tx: {}", PQerrorMessage(conn));
                PQclear(res);
                PGresult* rollback_res = PQexec(conn, "ROLLBACK");
                if (rollback_res) {
                    PQclear(rollback_res);
                }
                return false;
            }
            PQclear(res);
        }

        // Commit transaction
        res = PQexec(conn, "COMMIT");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            logger.errorf("BlockIndexer: Failed to commit rollback transaction: {}", PQerrorMessage(conn));
            PQclear(res);
            return false;
        }
        PQclear(res);

        logger.info("BlockIndexer: Reorg rollback complete:");
        logger.infof("  - Rolled back to height: {}", target_height);
        logger.infof("  - Blocks removed: {}", affected_block_count);
        logger.infof("  - Transactions removed: {}", affected_tx_count);

        // Store reorg depth atomically (lock-free operation)
        last_reorg_depth.store(progress_current_height - target_height, std::memory_order_release);
        return true;

    } catch (const std::exception& e) {
        logger.errorf("BlockIndexer: Exception during rollback: {}", e.what());
        PGresult* rollback_res = PQexec(conn, "ROLLBACK");
        if (rollback_res) {
            PQclear(rollback_res);
        }
        return false;
    }
}

bool BlockIndexer::record_reorg_event(int reorg_height, int reorg_depth) {
    PGconn* conn = static_cast<PGconn*>(db_connection_);
    if (!conn) {
        return false;
    }

    std::string query =
        "SELECT record_reorg_event($1, $2, NULL, NULL, 0, 0, 0)";

    std::string reorg_height_str = std::to_string(reorg_height);
    std::string reorg_depth_str = std::to_string(reorg_depth);

    const char* param_values[] = {
        reorg_height_str.c_str(),
        reorg_depth_str.c_str()
    };

    PGresult* res = PQexecParams(conn, query.c_str(), 2, nullptr, param_values, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        logger.warnf("BlockIndexer: Failed to record reorg event: {}", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    logger.infof("BlockIndexer: Reorg event recorded - height: {}, depth: {}",
               reorg_height, reorg_depth);
    return true;
}


// Producer thread: Fetches blocks and transactions from RPC in batches and queues them for database insertion
// This runs independently of the consumer thread to enable parallel RPC fetching and DB inserts
void BlockIndexer::thread_func_rpc_producer(std::stop_token stop_token, batch_type bt, int start, int end)
{
    // Set thread name for debugging
    pthread_setname_np(pthread_self(), "Inzyght-RPC-producer");

    logger.infof("BlockIndexer: RPC Producer thread started (fetching batch of {} from {} to {})", batch_type_string(bt), start, end);

    if (bt == BT_HEADERS)
    {
        try
        {
            for (int batch_start = start; batch_start <= end && !stop_token.stop_requested(); )
            {
                // Query current recommended batch size from RPC client (may be reduced due to fat blocks)
                int current_batch_size = rpc_client->get_recommended_batch_size();
                int batch_end = std::min(batch_start + current_batch_size - 1, end);

                // Prepare heights for this batch
                std::vector<int> heights;
                for (int h = batch_start; h <= batch_end && !stop_token.stop_requested(); ++h)
                {
                    heights.emplace_back(h);
                }

                logger.debugf("BlockIndexer: Producer fetching data {} - {} (batch size: {})",
                            batch_start, batch_end, batch_end - batch_start + 1);

                // Fetch all block headers for this batch from RPC with retry logic for transient failures
                auto fetch_start = std::chrono::high_resolution_clock::now();
                
                std::vector<YcashBlock> block_basic_results;
                block_basic_results.reserve(heights.size());

                int retry_count = 0;
                const int MAX_RETRIES = 3;
                bool fetch_success = false;

                while (retry_count < MAX_RETRIES && !stop_token.stop_requested())
                {
                    block_basic_results = rpc_client->batch_get_block_headers(heights, 1);

                    if (block_basic_results.size() == heights.size())
                    {
                        fetch_success = true;
                        break;
                    }

                    retry_count++;
                    if (retry_count < MAX_RETRIES)
                    {
                        int backoff_ms = 100 * (1 << (retry_count - 1));  // Exponential: 100ms, 200ms, 400ms
                        logger.warnf("BlockIndexer: Producer - RPC fetch failed (attempt {}/{}), retrying in {} ms",retry_count, MAX_RETRIES, backoff_ms);
                        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    }
                }

                if (stop_token.stop_requested())
                {
                    flag_producer_stopped = true;
                    queue_cv.notify_all();
                    return;
                }

                if (!fetch_success || block_basic_results.size() != heights.size())
                {
                    logger.errorf("BlockIndexer: Producer - Batch data fetch failed after {} retries", MAX_RETRIES);
                    flag_producer_stopped = true;
                    queue_cv.notify_all();
                    return;
                }

                auto headers_end = std::chrono::high_resolution_clock::now();
                uint64_t headers_ms = std::chrono::duration_cast<std::chrono::milliseconds>(headers_end - fetch_start).count();

                // Let's try to reach some more info here, i.e. each block miner address

                std::vector<Transaction> cb_results {};
                std::vector<std::pair<int, std::string>> cb_txes {};
                cb_txes.reserve(block_basic_results.size());

                for (const auto &one_block_result : block_basic_results)
                {
                    if (one_block_result.height > 0) // skip genesis
                    {
                        cb_txes.emplace_back(one_block_result.height, one_block_result.tx[0]);
                    }
                }

                // reset retry counter and success flag for this rpc call
                retry_count = 0;
                fetch_success = false;

                while (retry_count < MAX_RETRIES && !stop_token.stop_requested())
                {
                    cb_results = rpc_client->batch_get_coinbase_transactions(cb_txes);

                    if (cb_results.size() == cb_txes.size())
                    {
                        fetch_success = true;
                        break;
                    }

                    retry_count++;
                    if (retry_count < MAX_RETRIES)
                    {
                        int backoff_ms = 100 * (1 << (retry_count - 1));  // Exponential: 100ms, 200ms, 400ms
                        logger.warnf("BlockIndexer: Producer - RPC fetch failed (attempt {}/{}), retrying in {} ms",retry_count, MAX_RETRIES, backoff_ms);
                        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    }
                }

                // check if shutdown has been requested meanwhile
                if (stop_token.stop_requested())
                {
                    flag_producer_stopped = true;
                    queue_cv.notify_all();
                    return;
                }

                auto cb_end = std::chrono::high_resolution_clock::now();
                uint64_t cb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cb_end - headers_end).count();


                bool flag_fetch_subsidies {false};

                if (flag_fetch_subsidies)
                {
                    // fetch block subsidies also
                    std::vector<BlockSubsidy> subsidy_results {};
                    subsidy_results.reserve(heights.size());


                    // reset retry counter and success flag for this rpc call
                    retry_count = 0;
                    fetch_success = false;

                    while (retry_count < MAX_RETRIES && !stop_token.stop_requested())
                    {
                        subsidy_results = rpc_client->batch_get_block_subsidies(heights);

                        if (subsidy_results.size() == (heights[0] == 0) ? heights.size() - 1 : heights.size())
                        {
                            fetch_success = true;
                            break;
                        }

                        retry_count++;
                        if (retry_count < MAX_RETRIES)
                        {
                            int backoff_ms = 100 * (1 << (retry_count - 1));  // Exponential: 100ms, 200ms, 400ms
                            logger.warnf("BlockIndexer: Producer - RPC fetch failed (attempt {}/{}), retrying in {} ms", retry_count, MAX_RETRIES, backoff_ms);
                            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                        }
                    }

                    // check if shutdown has been requested meanwhile
                    if (stop_token.stop_requested())
                    {
                        flag_producer_stopped = true;
                        queue_cv.notify_all();
                        return;
                    }
                }

                auto fetch_end = std::chrono::high_resolution_clock::now();
                uint64_t fetch_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fetch_end - fetch_start).count();

                logger.infof("RPC: Batch {} - {} | Headers = {} ms ({} blocks) | CoinbaseTxs = {} ms ({} txs) | Total = {} ms",
                    batch_start, batch_end, headers_ms, block_basic_results.size(), cb_ms, cb_results.size(), fetch_time_ms);

                // Create batch structure and queue it
                FetchedDataBatch batch_of_headers;
                batch_of_headers.bt = BT_HEADERS;
                batch_of_headers.start_height = batch_start;
                batch_of_headers.end_height = batch_end;
                batch_of_headers.heights = heights;
                batch_of_headers.fetched_data = std::move(block_basic_results);
                batch_of_headers.extra_data = std::move(cb_results);
                batch_of_headers.fetch_time_ms = fetch_time_ms;
                batch_of_headers.fetch_complete_time = fetch_end;           

                // Wait if queue is too full (backpressure)
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    backpressure_cv.wait(lock, [this, &stop_token]() {
                        // Resume when: queue has space OR stopping OR consumer has died
                        return pending_batch_count.load(std::memory_order_acquire) < max_queue_depth
                            || stop_token.stop_requested()
                            || flag_consumer_failed.load(std::memory_order_acquire);
                    });
                }

                if (stop_token.stop_requested())
                {
                    logger.info("BlockIndexer: RPC Producer thread stopped during backpressure wait");
                    break;
                }

                if (flag_consumer_failed.load(std::memory_order_acquire))
                {
                    logger.error("BlockIndexer: RPC Producer aborting because consumer thread failed");
                    break;
                }

                // Queue the batch
                {
                    std::scoped_lock<std::mutex> lock(queue_mutex);
                    data_queue.emplace_back(std::move(batch_of_headers));
                }

                pending_batch_count.fetch_add(1, std::memory_order_release);

                if (pending_batch_count == 1)
                {
                    queue_cv.notify_one();  // Only notify if queue transitioned from empty
                }

                logger.debugf("BlockIndexer: Producer queued batch {} - {} (fetch time: {} ms, queue size: {})", batch_start, batch_end, fetch_time_ms, pending_batch_count.load());

                // Advance to next batch (using dynamic batch size)
                batch_start += current_batch_size;
            }
        }
        catch (const std::exception& e)
        {
            logger.errorf("BlockIndexer: Producer thread exception: {}", e.what());
        }
    }

    flag_producer_stopped = true;
    queue_cv.notify_all();  // Wake up consumer in case it's waiting
    logger.info("BlockIndexer: RPC Producer thread finished");
}


// Consumer thread: Processes fetched data from queue and inserts into database
void BlockIndexer::thread_func_db_consumer(std::stop_token stop_token)
{
    // Set thread name for debugging
    pthread_setname_np(pthread_self(), "Inzyght-db-consumer");

    logger.info("BlockIndexer: DB Consumer thread started");

    // Reused across batches so the underlying allocation amortizes over many
    // batches instead of being freed and re-grown each time.
    BinaryCopyBuffer block_copy_buf;
    BinaryCopyBuffer tx_copy_buf;

    try
    {
        while (!stop_token.stop_requested())
        {
            FetchedDataBatch fetched_data_batch;
            bool has_batch = false;
            uint64_t queue_wait_ms = 0;  // Time actually spent inside cv.wait (not lock + pop)

            // Wait for next batch from producer
            {
                std::unique_lock<std::mutex> lock(queue_mutex);

                // Wait until queue has data or producer has stopped or shutdown requested
                auto queue_wait_start = std::chrono::high_resolution_clock::now();
                queue_cv.wait(lock, [this, &stop_token]() {
                    return !data_queue.empty() || flag_producer_stopped || stop_token.stop_requested();
                });
                queue_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - queue_wait_start).count();

                if (!data_queue.empty())
                {
                    fetched_data_batch = std::move(data_queue.front());
                    data_queue.pop_front();
                    has_batch = true;
                }
                else if (flag_producer_stopped || stop_token.stop_requested())
                {
                    // No more batches and producer finished, OR shutdown requested
                    break;
                }
            }

            if (!has_batch)
            {
                continue;
            }

            int new_count = pending_batch_count.fetch_sub(1, std::memory_order_release) - 1;

            if (new_count < max_queue_depth)
            {
                backpressure_cv.notify_one();
            }

            // queue_residence_ms: how long this batch sat in the queue between
            // producer enqueue and consumer pickup. Large values when the queue
            // is full mean the consumer is the bottleneck — that is normal and
            // expected, not a latency problem.
            auto processing_start = std::chrono::high_resolution_clock::now();
            auto queue_residence_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                processing_start - fetched_data_batch.fetch_complete_time).count();

            logger.debugf("BlockIndexer: Consumer processing batch {} - {} (queue wait: {} ms, queue residence: {} ms, queue size: {})",
                          fetched_data_batch.start_height, fetched_data_batch.end_height,
                          queue_wait_ms, queue_residence_ms, pending_batch_count.load());

            auto db_start = processing_start;
            BatchTimingData timing;
            timing.block_count = fetched_data_batch.fetched_data.size();
            timing.rpc_ms = fetched_data_batch.fetch_time_ms;

            // Process all data in this batch
            if (fetched_data_batch.bt == BT_HEADERS)
            {
                const std::vector<YcashBlock> &local_fetched_data = fetched_data_batch.fetched_data;
                const std::optional<std::vector<Transaction>> &local_extra_data = fetched_data_batch.extra_data;

                const bool extract_miner_address {local_extra_data.has_value()};
                std::unordered_map<int, CoinbaseInfo> miners_by_height {};

                if (extract_miner_address)
                {
                    for (const Transaction& cb_tx : local_extra_data.value())
                    {
                        const int block_height = cb_tx.height;
                        std::string miner_address {};
                        int64_t claimed_reward {0};
                        int64_t claimed_reward_block {0};

                        if (cb_tx.valueBalanceZat < 0)
                        {
                            claimed_reward = 0 - cb_tx.valueBalanceZat;
                            claimed_reward_block = claimed_reward;
                            miner_address = "private";
                        }

                        for (const TransactionOutput& one_out : cb_tx.vout)
                        {
                            claimed_reward_block += one_out.valueZat;

                            if (one_out.valueZat > claimed_reward)
                            {
                                claimed_reward = one_out.valueZat;
                                try
                                {
                                    miner_address = one_out.scriptPubKey.addresses.at(0);
                                }
                                catch (...)
                                {
                                    miner_address = "unknown";
                                }
                            }
                        }
                        CoinbaseInfo cbi {.miner_address = miner_address, .claimed_reward_miner = claimed_reward, .claimed_reward_block = claimed_reward_block};
                        miners_by_height.try_emplace(block_height, cbi);
                    }
                }

                // Build PGCOPY binary streams (much faster: no decimal/hex
                // formatting on the client, no text parsing on the server,
                // ~50% smaller wire payload). Buffers are reused across
                // batches; clear() keeps the backing storage.
                block_copy_buf.clear();
                tx_copy_buf.clear();
                block_copy_buf.write_header();
                tx_copy_buf.write_header();

                int tx_index = 0;

                for (const YcashBlock& block_result : local_fetched_data)
                {
                    const int     height    = block_result.height;
                    const int64_t timestamp = block_result.time;

                    std::string miner_address {};
                    int64_t     claimed_reward_miner {0};
                    int64_t     claimed_reward_block {0};

                    if (extract_miner_address)
                    {
                        auto locator = miners_by_height.find(height);
                        if (locator != miners_by_height.end())
                        {
                            miner_address        = locator->second.miner_address;
                            claimed_reward_miner = locator->second.claimed_reward_miner;
                            claimed_reward_block = locator->second.claimed_reward_block;
                        }
                    }
                    // Schema column is varchar(50); enforce client-side so a
                    // single oversize value cannot reject the whole batch.
                    if (miner_address.size() > 50) miner_address.resize(50);

                    // blocks: 14 columns
                    block_copy_buf.begin_row(14);
                    block_copy_buf.write_int4(height);
                    if (!block_copy_buf.write_bytea_from_hex64(block_result.hash)) {
                        logger.errorf("BlockIndexer: Consumer - invalid block hash at height {}: '{}'", height, block_result.hash);
                        flag_consumer_failed = true;
                        backpressure_cv.notify_all();
                        return;
                    }
                    // previousblockhash may be empty (genesis); write empty bytea to mirror prior behaviour.
                    if (block_result.previousblockhash.empty()) {
                        block_copy_buf.write_bytea(nullptr, 0);
                    } else if (!block_copy_buf.write_bytea_from_hex64(block_result.previousblockhash)) {
                        logger.errorf("BlockIndexer: Consumer - invalid previous_hash at height {}: '{}'", height, block_result.previousblockhash);
                        flag_consumer_failed = true;
                        backpressure_cv.notify_all();
                        return;
                    }
                    block_copy_buf.write_int8(timestamp);
                    block_copy_buf.write_float8(block_result.difficulty);
                    block_copy_buf.write_int4(block_result.size);
                    block_copy_buf.write_int4(static_cast<int32_t>(block_result.tx.size()));
                    block_copy_buf.write_int8(block_result.chainSupply.chainValueZat);
                    block_copy_buf.write_int8(block_result.valuePools[0].chainValueZat);
                    block_copy_buf.write_int8(block_result.valuePools[1].chainValueZat);
                    block_copy_buf.write_int8(block_result.valuePools[2].chainValueZat);
                    block_copy_buf.write_text(miner_address);
                    block_copy_buf.write_int8(claimed_reward_block);
                    block_copy_buf.write_int8(claimed_reward_miner);

                    // transactions: 5 columns (hash, block_height, tx_index, timestamp, is_coinbase)
                    const std::vector<std::string>& transactions = block_result.tx;
                    for (size_t i = 0; i < transactions.size(); ++i)
                    {
                        tx_copy_buf.begin_row(5);
                        if (!tx_copy_buf.write_bytea_from_hex64(transactions[i])) {
                            logger.errorf("BlockIndexer: Consumer - invalid tx hash at height {} idx {}: '{}'", height, i, transactions[i]);
                            flag_consumer_failed = true;
                            backpressure_cv.notify_all();
                            return;
                        }
                        tx_copy_buf.write_int4(height);
                        tx_copy_buf.write_int4(static_cast<int32_t>(i));
                        tx_copy_buf.write_int8(timestamp);
                        tx_copy_buf.write_bool(i == 0);

                        tx_index++;
                    }
                }

                block_copy_buf.write_trailer();
                if (tx_index > 0) tx_copy_buf.write_trailer();

                // End of prep phase (binary encoding); start of libpq COPY phase.
                auto copy_start = std::chrono::high_resolution_clock::now();
                timing.prep_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    copy_start - db_start).count();

                // Atomic write: blocks, their transactions, and the persisted
                // headers pointer all commit together (or all roll back). This
                // closes the torn-write window where a crash between the two
                // COPYs left blocks without their transactions — which, because
                // restart resumes from MAX(blocks.height), could never be
                // backfilled. A rolled-back sub-batch is simply re-fetched from
                // RPC on the next pass.
                try
                {
                    CopyTxn txn;

                    std::vector<std::string> block_columns {"height", "hash", "previous_hash", "timestamp", "difficulty", "size", "tx_count", "chain_supply", "transparent_supply", "sprout_supply", "sapling_supply", "miner_address", "claimed_reward_block", "claimed_reward_miner"};

                    CopyResult block_result = txn.copy_binary("blocks", block_columns, block_copy_buf);
                    logger.debugf("BlockIndexer: Consumer - COPY inserted {} blocks", block_result.rows_affected);

                    if (tx_index > 0)
                    {
                        std::vector<std::string> tx_columns = {"hash", "block_height", "tx_index", "timestamp", "is_coinbase"};

                        CopyResult tx_result = txn.copy_binary("transactions", tx_columns, tx_copy_buf);
                        logger.debugf("BlockIndexer: Consumer - COPY inserted {} transactions", tx_result.rows_affected);
                    }

                    // Persist the headers pointer inside the same transaction so
                    // sync_progress can never point past committed blocks/txs.
                    int total_h = progress_total_height.load(std::memory_order_acquire);
                    if (total_h < fetched_data_batch.end_height) total_h = fetched_data_batch.end_height;
                    txn.exec(std::format(
                        "SELECT update_sync_progress('headers', {}, {}, {}, 'syncing', '')",
                        fetched_data_batch.end_height, total_h, fetched_data_batch.end_height));

                    txn.commit();
                }
                catch (const std::exception& e)
                {
                    logger.errorf("BlockIndexer: Consumer - atomic block+tx COPY failed: {}", e.what());
                    flag_consumer_failed = true;
                    backpressure_cv.notify_all();
                    return;
                }

                auto db_end = std::chrono::high_resolution_clock::now();
                timing.copy_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(db_end - copy_start).count();
                // total_ms is the wall-clock cost of one consumer iteration.
                // rpc_ms is NOT added here: in the parallel pipeline the producer's
                // fetch for batch N+1 overlaps with the consumer's work on batch N,
                // so summing it would double-count and understate throughput.
                timing.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(db_end - db_start).count();

                logger.infof("TIMING: Batch {} - {} | QueueWait = {} ms | QueueResidence = {} ms | RPC = {} ms | Prep = {} ms | Copy = {} ms | Total = {} ms",
                        fetched_data_batch.start_height, fetched_data_batch.end_height,
                        queue_wait_ms, queue_residence_ms, timing.rpc_ms,
                        timing.prep_ms, timing.copy_ms, timing.total_ms);

                // Save batch timing data for current_speed calculation
                {
                    std::scoped_lock<std::mutex> lock(timing_mutex);
                    batch_timing = timing;
                }

                progress_current_height = fetched_data_batch.end_height;
                record_speed_sample(fetched_data_batch.end_height);
                int current_height = progress_current_height;
                int window_start = progress_recent_window_start_height;

                // Slide the window forward if it's grown beyond RECENT_WINDOW_SIZE
                if (current_height - window_start > RECENT_WINDOW_SIZE)
                {
                    // Calculate the new window start height (last 1000 blocks)
                    int new_window_start = current_height - RECENT_WINDOW_SIZE;

                    // CRITICAL: Update the timestamp to NOW minus the time for recent blocks
                    // We use the overall average speed to estimate when we were at new_window_start
                    int64_t sync_start_time = progress_sync_start_time;
                    int sync_start_height = progress_sync_start_height;
                    int64_t current_time = std::time(nullptr);

                    if (sync_start_time > 0 && sync_start_height >= 0)
                    {
                        // Calculate overall average speed
                        int64_t total_elapsed = current_time - sync_start_time;
                        int total_blocks = current_height - sync_start_height;

                        if (total_elapsed > 0 && total_blocks > 0)
                        {
                            // Estimate: how long ago were we at new_window_start?
                            int blocks_ago = current_height - new_window_start;
                            double avg_speed = (double)total_blocks / (double)total_elapsed;
                            int64_t seconds_ago = (int64_t)((double)blocks_ago / avg_speed);

                            // Set window start time to (now - seconds_ago)
                            progress_recent_window_start_time = current_time - seconds_ago;
                        }
                        else
                        {
                            progress_recent_window_start_time = current_time;
                        }
                    }
                    else
                    {
                        progress_recent_window_start_time = current_time;
                    }

                    progress_recent_window_start_height = new_window_start;
                }

                // Explicitly free batch data to ensure memory is returned to OS
                fetched_data_batch.heights.clear();
                fetched_data_batch.heights.shrink_to_fit();
                fetched_data_batch.fetched_data.clear();
                fetched_data_batch.fetched_data.shrink_to_fit();
            }
            else
            {
                logger.warn("Unknown batch type fetched !!!");
            }
            
        }
    }
    catch (const std::exception& e)
    {
        logger.errorf("BlockIndexer: Consumer thread exception: {}", e.what());
        flag_consumer_failed = true;
        backpressure_cv.notify_all();
    }

    // Defensive: ensure producer is never left blocked on backpressure_cv after consumer exit.
    // Normal (queue-drained) exits are also covered here.
    backpressure_cv.notify_all();

    logger.info("BlockIndexer: DB Consumer thread finished");
}
