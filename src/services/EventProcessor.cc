#include "EventProcessor.h"
#include "ZeroMQListener.h"
#include "EventBroadcaster.h"
#include "BlockchainState.h"
#include "BlockIndexer.h"
#include "Logger.h"
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
                logger.debugf("Processing transaction event: hash={}", event.data);
                process_transaction_event(event.data);
            }
            else
            {
                // Log unknown events with full details
                logger.warnf("Received unknown ZeroMQ event type '{}' with data: {} (data_length: {} hex chars / {} bytes)", event.type, event.data, event.data.length(), event.data.length() / 2);
            }
        }

        // Only sleep if no events were found
        if (!has_event)
        {
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
        BlockchainState::instance().update_state_on_block_event();

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

        // Broadcast network info from cached state (no RPC call)
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
    catch (const std::exception& e)
    {
        logger.errorf("EventProcessor process_block_event error: {}", e.what());
    }
}

void EventProcessor::process_transaction_event(const std::string& tx_hash)
{
    Logger& logger = Logger::instance();
    auto start_time = std::chrono::high_resolution_clock::now();

    if (BlockIndexer::instance().is_syncing())
    {
        logger.debugf("Skipping transaction broadcast during sync: {}", tx_hash);
        return;
    }

    try
    {
        BlockchainState::instance().update_state_on_transaction_event();

        // Notify WebSocket clients that a new transaction arrived; frontend will re-fetch via API
        glz::generic tx_data;
        tx_data["txid"] = tx_hash;
        EventBroadcaster::instance().broadcast_transaction_update(tx_data);

        // Update mempool count in cached network info and broadcast
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
        logger.infof("Transaction event broadcasted: {} (took {}ms)", tx_hash, elapsed);
    }
    catch (const std::exception& e)
    {
        logger.errorf("EventProcessor process_transaction_event error: {}", e.what());
    }
}
