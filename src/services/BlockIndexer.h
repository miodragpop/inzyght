#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <deque>
#include <vector>
#include <utility>
#include <condition_variable>
#include "RpcConfig.h"
#include "models/RpcResponses.h"

// Forward declarations
class YcashRpcClient;

struct CoinbaseInfo {
    std::string miner_address {};
    int64_t claimed_reward_miner {};
    int64_t claimed_reward_block {};
};


// Per-batch timing measurements for the indexer pipeline.
//
//   rpc_ms        — producer wall time around the RPC fetch (overlaps with
//                   prior batch's DB work; do NOT sum into total_ms).
//   prep_ms       — consumer time spent serializing block + tx rows into the
//                   COPY string format.
//   copy_ms       — consumer time spent inside the libpq COPY calls
//                   (execute_copy("blocks") + execute_copy("transactions")).
//   total_ms      — wall-clock cost of one consumer iteration from
//                   processing_start to db_end. This is the per-batch
//                   pipeline cost when the queue is non-empty.
//   block_count   — number of blocks in the batch.
struct BatchTimingData {
    uint64_t rpc_ms = 0;
    uint64_t prep_ms = 0;
    uint64_t copy_ms = 0;
    uint64_t total_ms = 0;
    int block_count = 0;
};

enum batch_type {
    BT_HEADERS
};

std::string batch_type_string(batch_type);

struct FetchedDataBatch {
    batch_type bt;
    int start_height {0};
    int end_height {0};
    std::vector<int> heights {};
    std::vector<YcashBlock> fetched_data {};
    std::optional<std::vector<Transaction>> extra_data {};
    uint64_t fetch_time_ms {0};
    std::chrono::high_resolution_clock::time_point fetch_complete_time;  // Timestamp when RPC fetch completed
};


class BlockIndexer
{
    public:
        // Singleton instance
        static BlockIndexer& instance();

        // Initialize indexer with configuration
        bool initialize(const RpcConfig::Config& config);

        // Start background indexing thread (performs startup check for missing blocks)
        void start();

        // Stop background indexing thread
        void stop();

        // Perform startup check: calculate difference between last indexed and current chain tip
        // Returns true if catchup is needed, false if already synced
        bool perform_startup_check();

        // Check if indexer is running
        bool is_running() const { return flag_running; }

        // Check if indexer is syncing (not fully caught up)
        bool is_syncing() const { return flag_syncing; }

        // Number of blocks the indexer is behind the chain tip
        int blocks_behind() const { return progress_total_height - progress_current_height; }

        // Get current sync progress (percentage)
        double get_sync_progress() const;

        // Get current indexed height
        int get_current_height() const;

        // Get average sync speed (blocks per second) - total blocks / total elapsed time from sync start
        double get_average_sync_speed() const;

        // Get current batch speed (blocks per second) - blocks in last batch / time to process batch
        double get_current_batch_speed() const;

        // Get estimated time to completion (seconds)
        int64_t get_estimated_time_remaining() const;

        // Get sync start time (Unix timestamp)
        int64_t get_sync_start_time() const { return progress_sync_start_time; }

        // Destructor
        ~BlockIndexer();

    private:
        BlockIndexer();

        // Helper: Sleep with cancellation checks
        // Returns false if stop signal received, true if sleep completed
        bool interruptible_sleep(std::stop_token stop_token, int seconds);

        // Helper: Wait for new blocks via ZeroMQ events with timeout fallback
        // Returns false if stop signal received, true if wait completed (with or without event)
        bool wait_for_new_blocks_or_timeout(std::stop_token stop_token, int timeout_seconds);

        // Background indexing thread function
        void thread_func_indexer(std::stop_token stop_token);

        // Producer thread: Fetches data from RPC and queues them
        void thread_func_rpc_producer(std::stop_token stop_token, batch_type bt, int start, int end);

        // Consumer thread: Processes fetched blocks and inserts into database
        void thread_func_db_consumer(std::stop_token stop_token);

        // Batch data indexing (parallel implementation using producer-consumer threads)
        bool index_chain_data_batch(std::stop_token stop_token, batch_type bt, int start_height, int end_height);

        // Update sync progress in database
        void update_sync_progress(batch_type bt, int current_height, int total_height, std::string_view status, std::string_view error_msg = "");

        // Refresh transaction count cache from database (if cache is stale)
        void refresh_transaction_count_cache();

        // Get current blockchain height from RPC
        int get_blockchain_height();

        // Get last indexed height from DB
        int get_last_indexed_height();

        // Get current transactions count from DB
        int get_transactions_count() const;

        // Get indexed transactions count from DB
        int get_indexed_transactions_count() const;

        // Initialize database connection
        bool initialize_database();

        // Startup integrity repair: find the lowest block whose committed
        // transaction count disagrees with its recorded tx_count (a torn write
        // from a pre-fix crash) and roll the chain back to just before it so the
        // normal catchup re-indexes the affected range cleanly. Returns the
        // height repaired back to, or -1 if the chain is already consistent.
        int repair_torn_transaction_writes();

        // Fill one chunk of the lowest historical block gap (holes left below the
        // tip by earlier partial-batch failures). Re-indexes the missing heights
        // through the normal pipeline without touching the forward-sync pointer.
        // Returns true if a gap chunk was processed (more may remain), false when
        // no gaps are left to fill. Called while idle at the tip.
        bool backfill_next_gap(std::stop_token stop_token);

        // Reorg detection and handling (for chain reorganizations)
        bool detect_reorg(int height_to_check);
        int find_common_ancestor(int start_height);
        bool rollback_to_height(int target_height);
        bool record_reorg_event(int reorg_height, int reorg_depth);

        // Helper methods for reorg handling
        std::string get_block_hash_from_rpc(int height);
        std::string get_block_hash_from_db(int height);

        // Configuration
        RpcConfig::Config rpc_config;

        // Database connection (void* to avoid pgsql header dependency)
        void* db_connection_;

        // RPC client (pointer to singleton main instance)
        YcashRpcClient* rpc_client;

        // Thread management
        std::jthread indexing_thread;
        std::atomic<bool> flag_running{false};
        std::atomic<bool> flag_syncing{false};  // True when actively catching up, false when fully synced
        std::atomic<bool> flag_backfilling{false};  // True while filling a historical gap (suppresses forward-sync progress writes)

        // Historical gap backfill state (lazily loaded once, drained over idle cycles)
        std::vector<std::pair<int, int>> backfill_ranges_;  // [start,end] missing ranges, ascending
        size_t backfill_cursor_ = 0;
        bool backfill_loaded_ = false;
        static constexpr int BACKFILL_CHUNK = 10000;  // max heights filled per backfill_next_gap() call

        // Progress tracking (all atomic - no mutex needed for single-writer main thread)
        std::atomic<int> progress_current_height{0};
        std::atomic<int> progress_total_height{0};
        std::atomic<int> progress_sync_start_height{0};
        std::atomic<int64_t> progress_sync_start_time{0};

        // Rolling window for recent sync speed calculation
        std::atomic<int> progress_recent_window_start_height{0};  // Height at start of last 1000 blocks
        std::atomic<int64_t> progress_recent_window_start_time{0};  // Timestamp at start of last 1000 blocks
        static constexpr int RECENT_WINDOW_SIZE = 2000;  // Track last 2000 blocks for recent speed

        // Transaction sync progress tracking
        std::atomic<int> progress_tx_sync_start_height{0};
        std::atomic<int64_t> progress_tx_sync_start_time{0};
        std::atomic<int> progress_tx_recent_window_start_height{0};  // Height at start of recent window
        std::atomic<int64_t> progress_tx_recent_window_start_time{0};  // Timestamp at start of recent window
        static constexpr int TX_RECENT_WINDOW_SIZE = 100;  // Track last 100 transactions for recent speed (initialize quickly)

        // Cached transaction counts (updated during indexing, read-only from API)
        std::atomic<int> cached_tx_current_height{0};  // Currently indexed transaction count
        std::atomic<int> cached_tx_total_height{0};    // Total transactions to index
        std::atomic<int64_t> cached_tx_last_update_time{0};  // Unix timestamp of last cache update

        // Batch size for indexing (blocks per batch from main loop)
        // Inner batching: number of block headers per database transaction is set in inzyght.conf
        static constexpr int HEADERS_BATCH_SIZE = 100000;

        // Batch size for indexing (transactions per batch from main loop)
        // Inner batching: number of transactions per database transaction is set dynamicaly, related to estimated tx sizes
        static constexpr int TRANSACTIONS_BATCH_SIZE = 50000;

        static constexpr int MAX_REORG_DEPTH = 100;

        // Reorg tracking
        mutable std::mutex reorg_mutex;
        std::atomic<int> last_reorg_depth{0};  // Lock-free - no mutex needed for this value

        // Batch timing data for profiling
        BatchTimingData batch_timing;
        mutable std::mutex timing_mutex;

        // Trailing-window samples for "current speed" — each sample records
        // the consumer's wall-clock time and the height reached after a batch.
        // The UI's "current speed" is computed as
        //   (newest.height - oldest.height) / (newest.time - oldest.time)
        // which naturally settles to 0 when the indexer is idle at the tip
        // (no new samples, oldest sample ages out, numerator → 0). Sample
        // count is small (recent ~60s) so reads are O(N) but N is tiny.
        struct SpeedSample {
            int64_t time;   // Unix seconds
            int     height;
        };
        static constexpr size_t SPEED_RING_CAPACITY = 64;
        static constexpr int    SPEED_WINDOW_SECONDS = 60;
        mutable std::mutex speed_ring_mutex;
        SpeedSample speed_ring[SPEED_RING_CAPACITY] {};
        size_t      speed_ring_head{0};   // next write slot
        size_t      speed_ring_count{0};  // valid samples

        // Record a (time, height) sample after a batch is committed.
        void record_speed_sample(int height);

        // Parallel indexing: Producer-consumer queue for RPC fetched blocks
        std::deque<FetchedDataBatch> data_queue;
        mutable std::mutex queue_mutex;
        std::condition_variable queue_cv;  // Signal when data available or queue stopping
        std::condition_variable backpressure_cv;  // Signal when space becomes available in queue (producer backpressure relief)
        std::atomic<bool> flag_producer_stopped{false};  // Signal that producer has finished
        std::atomic<bool> flag_consumer_failed{false};  // Consumer aborted on error; producer must stop blocking on backpressure
        std::atomic<int> pending_batch_count{0};  // Track number of queued batches for backpressure
        int max_queue_depth{5};  // Max batches queued (configurable, from config file)

        // Thread management for parallel processing
        std::jthread rpc_producer_thread;
        std::jthread db_consumer_thread;
};