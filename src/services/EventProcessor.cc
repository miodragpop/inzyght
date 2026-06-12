#include "EventProcessor.h"
#include "ZeroMQListener.h"
#include "EventBroadcaster.h"
#include "BlockchainState.h"
#include "BlockIndexer.h"
#include "RpcResponseCache.h"
#include "Logger.h"
#include "ycash_rpc_client.h"
#include "glaze/json/generic.hpp"
#include "models/RpcResponses.h"
#include <chrono>

EventProcessor& EventProcessor::instance()
{
    static EventProcessor instance;
    return instance;
}

EventProcessor::~EventProcessor()
{
    stop();
}

bool EventProcessor::initialize(const RpcConfig::Config& conf)
{
    config = conf;
    Logger::instance().info("EventProcessor service initialized");
    return true;
}

void EventProcessor::start()
{
    if (running)
    {
        return;
    }

    Logger::instance().info("Starting EventProcessor service");
    running = true;
    processor_thread = std::jthread(&EventProcessor::processor_thread_func, this);
}

void EventProcessor::stop()
{
    if (!running)
    {
        return;  // Already stopped
    }

    Logger::instance().info("Stopping EventProcessor service");
    processor_thread.request_stop();

    if (processor_thread.joinable())
    {
        processor_thread.join();
    }

    running = false;
    Logger::instance().info("EventProcessor service stopped");
}

void EventProcessor::processor_thread_func(std::stop_token stop_token)
{
    Logger& logger = Logger::instance();
    logger.info("EventProcessor thread started");

    while (!stop_token.stop_requested())
    {
        // Check for events from ZeroMQ queue
        ZeroMQListener::Event event;
        bool has_event = false;

        // Transaction events are coalesced: each one would otherwise trigger
        // a full verbose getrawmempool, so during a tx burst we drain the
        // queue first and refresh the mempool snapshot once for the batch.
        std::vector<std::string> tx_batch;

        // Process all available events in queue without waiting
        // Note: Individual event handlers will skip expensive RPC calls during sync
        while (ZeroMQListener::instance().pop_event(event))
        {
            has_event = true;

            if (event.type == "block")
            {
                logger.debugf("Processing block event: hash={}", event.data);
                process_block_event(event.data);
            }
            else if (event.type == "transaction")
            {
                logger.debugf("Queueing transaction event: hash={}", event.data);
                tx_batch.push_back(event.data);
            }
            else
            {
                // Log unknown events with full details
                logger.warnf("Received unknown ZeroMQ event type '{}' with data: {} (data_length: {} hex chars / {} bytes)", event.type, event.data, event.data.length(), event.data.length() / 2);
            }
        }

        if (!tx_batch.empty())
        {
            process_transaction_events(tx_batch);
        }

        // Only sleep if no events were found
        if (!has_event)
        {
            retry_state_refresh_if_needed();
            heartbeat_if_quiet();

            // No events, sleep briefly before checking again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    logger.info("EventProcessor thread ended");
}

void EventProcessor::process_block_event(const std::string& block_hash)
{
    Logger& logger = Logger::instance();
    auto start_time = std::chrono::high_resolution_clock::now();

    try
    {
        // New tip: everything tip-dependent in the RPC response cache is now
        // stale (confirmations, nextblockhash, address balances, mempool...).
        RpcResponseCache::instance().on_block_event();

        state_refresh_needed = !BlockchainState::instance().update_state_on_block_event();

        // Broadcast when at or near the tip (≤2 blocks behind covers the block currently being indexed)
        if (BlockIndexer::instance().blocks_behind() <= 2)
        {
            // Notify WebSocket clients that a new block arrived; frontend will re-fetch via API
            glz::generic::object_t block_data;
            block_data["hash"] = block_hash;
            EventBroadcaster::instance().broadcast_block_update(block_data);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
            logger.infof("Block event broadcasted: {} (took {}ms)", block_hash, elapsed);
        }
        else
        {
            logger.debugf("Skipping block broadcast during sync ({} blocks behind): {}", BlockIndexer::instance().blocks_behind(), block_hash);
        }

        broadcast_network_info_from_cache();
    }
    catch (const std::exception& e)
    {
        logger.errorf("EventProcessor process_block_event error: {}", e.what());
    }
}

void EventProcessor::broadcast_network_info_from_cache()
{
    Logger& logger = Logger::instance();
    try
    {
        const BlockchainState& current_state {BlockchainState::instance()};
        InfoResponseExtended info {current_state.get_cached_info_extended()};

        std::string json_str;
        auto ec = glz::write_json(info, json_str);
        if (!ec)
        {
            glz::generic info_result;
            ec = glz::read_json(info_result, json_str);
            if (!ec)
            {
                info_result["mempool_count"] = static_cast<int64_t>(BlockchainState::instance().get_mempool_count());
                BlockchainState::instance().set_proxy_network_info(info_result);
                EventBroadcaster::instance().broadcast_network_update(info_result);
                logger.debug("Network info broadcasted from cache");
            }
        }
    }
    catch (const std::exception& e)
    {
        logger.debugf("Failed to broadcast network info: {}", e.what());
    }
}

void EventProcessor::broadcast_recovery_update()
{
    if (BlockIndexer::instance().blocks_behind() > 2)
    {
        return;  // indexer still catching up; its block events will broadcast
    }

    try
    {
        glz::generic::object_t block_data;
        block_data["hash"] = BlockchainState::instance().get_cached_blockchain_info().bestblockhash;
        EventBroadcaster::instance().broadcast_block_update(block_data);

        // Synthetic empty txid: receivers only treat this as a "mempool may
        // have changed, re-fetch" signal.
        glz::generic tx_data;
        tx_data["txid"] = "";
        EventBroadcaster::instance().broadcast_transaction_update(tx_data);

        broadcast_network_info_from_cache();
        Logger::instance().info("EventProcessor: recovery broadcast sent to WebSocket clients");
    }
    catch (const std::exception& e)
    {
        Logger::instance().debugf("Recovery broadcast failed: {}", e.what());
    }
}

void EventProcessor::process_transaction_events(const std::vector<std::string>& tx_hashes)
{
    Logger& logger = Logger::instance();
    auto start_time = std::chrono::high_resolution_clock::now();

    if (BlockIndexer::instance().is_syncing())
    {
        logger.debugf("Skipping transaction broadcast during sync ({} events)", tx_hashes.size());
        return;
    }

    try
    {
        // One mempool refresh and one cache invalidation for the whole batch
        // (the refreshed snapshot already includes every tx in the batch).
        RpcResponseCache::instance().on_transaction_event();
        if (!BlockchainState::instance().update_state_on_transaction_event())
        {
            state_refresh_needed = true;
        }

        // Notify WebSocket clients per transaction; frontend will re-fetch via API
        for (const std::string& tx_hash : tx_hashes)
        {
            glz::generic tx_data;
            tx_data["txid"] = tx_hash;
            EventBroadcaster::instance().broadcast_transaction_update(tx_data);
        }

        // Update mempool count in cached network info and broadcast (once per batch)
        try
        {
            glz::generic net_info = BlockchainState::instance().get_proxy_network_info();
            if (!net_info.empty())
            {
                net_info["mempool_count"] = static_cast<int64_t>(BlockchainState::instance().get_mempool_count());
                BlockchainState::instance().set_proxy_network_info(net_info);
                EventBroadcaster::instance().broadcast_network_update(net_info);
            }
        }
        catch (const std::exception& e)
        {
            logger.debugf("Failed to broadcast mempool count update: {}", e.what());
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
        logger.infof("{} transaction event(s) broadcasted (took {}ms)", tx_hashes.size(), elapsed);
    }
    catch (const std::exception& e)
    {
        logger.errorf("EventProcessor process_transaction_events error: {}", e.what());
    }
}

void EventProcessor::retry_state_refresh_if_needed()
{
    // Also covers a failed refresh at startup (main.cc), which this class
    // never saw fail: age < 0 means no refresh has ever succeeded.
    if (!state_refresh_needed && BlockchainState::instance().get_snapshot_age_seconds() >= 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh_retry < kRefreshRetryInterval)
    {
        return;
    }
    last_refresh_retry = now;

    Logger& logger = Logger::instance();
    logger.info("EventProcessor: retrying BlockchainState refresh after failure");

    // The chain may have advanced while ycashd was unreachable, so cached
    // tip-dependent RPC responses from before the outage must not survive a
    // successful recovery.
    RpcResponseCache::instance().on_block_event();

    if (BlockchainState::instance().update_state_on_block_event())
    {
        state_refresh_needed = false;
        logger.info("EventProcessor: BlockchainState refresh recovered");
        broadcast_recovery_update();
    }
}

void EventProcessor::heartbeat_if_quiet()
{
    // A known failure is the retry loop's job; and age < 0 (never refreshed)
    // is covered there too.
    if (state_refresh_needed)
    {
        return;
    }

    const long age = BlockchainState::instance().get_snapshot_age_seconds();
    if (age < kHeartbeatAfterSeconds)
    {
        return;
    }

    Logger& logger = Logger::instance();
    try
    {
        const std::string tip = YcashRpcClient::thread_instance().get_best_block_hash().hash;
        const std::string cached_tip =
            BlockchainState::instance().get_cached_blockchain_info().bestblockhash;

        if (tip == cached_tip)
        {
            // Tip unchanged and no events arrived: the chain genuinely didn't
            // move, so the snapshot still matches reality — confirm freshness
            // without a full refresh. (In ZMQ fallback mode the mempool can
            // drift between tip changes; accepted limitation of polling mode.)
            BlockchainState::instance().confirm_snapshot_fresh();
            logger.debug("EventProcessor: heartbeat ok, snapshot confirmed fresh");
        }
        else
        {
            // Tip moved without a block event (missed ZMQ message or fallback
            // gap) — treat it like a block event.
            logger.infof("EventProcessor: heartbeat found new tip {} (missed block event?), refreshing", tip);
            RpcResponseCache::instance().on_block_event();
            state_refresh_needed = !BlockchainState::instance().update_state_on_block_event();
            if (!state_refresh_needed)
            {
                broadcast_recovery_update();
            }
        }
    }
    catch (const std::exception& e)
    {
        logger.warnf("EventProcessor: heartbeat probe failed: {}", e.what());
        BlockchainState::instance().mark_node_unreachable();
        state_refresh_needed = true;  // hand recovery to the retry loop
    }
}
