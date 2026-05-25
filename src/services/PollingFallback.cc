#include "PollingFallback.h"
#include "ZeroMQListener.h"
#include "EventBroadcaster.h"
#include "BlockchainState.h"
#include "BlockIndexer.h"
#include "ycash_rpc_client.h"
#include "Logger.h"
#include "glaze/json/read.hpp"
#include <chrono>


PollingFallback& PollingFallback::instance()
{
    static PollingFallback instance;
    return instance;
}

PollingFallback::PollingFallback() = default;

PollingFallback::~PollingFallback()
{
    stop();
}

bool PollingFallback::initialize(const RpcConfig::Config& conf)
{
    config = conf;
    Logger::instance().info("PollingFallback service initialized");
    return true;
}

void PollingFallback::start() {
    if (active)
    {
        return;
    }

    Logger& logger = Logger::instance();
    logger.info("Starting PollingFallback service");
    active = true;
    polling_thread = std::jthread(&PollingFallback::polling_thread_func, this);
}

void PollingFallback::stop()
{
    if (!active)
    {
        return;  // Already stopped
    }

    Logger& logger = Logger::instance();
    logger.info("Stopping PollingFallback service");

    polling_thread.request_stop();
    active = false;

    if (polling_thread.joinable())
    {
        polling_thread.join();
    }

    logger.info("PollingFallback service stopped");
}

void PollingFallback::polling_thread_func(std::stop_token stop_token)
{
    Logger& logger = Logger::instance();
    logger.info("PollingFallback thread started");

    while (!stop_token.stop_requested())
    {
        // Check if we should be polling
        if (!ZeroMQListener::instance().is_in_fallback_mode())
        {
            // ZeroMQ is working fine, wait before checking again
            // Sleep for 30 seconds total, but check stop_token every 100ms for responsive shutdown
            for (int i = 0; i < 300 && !stop_token.stop_requested(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        logger.info("Polling fallback active - fetching data from RPC");

        // Always poll network info (even during sync) to keep blockchain height updated
        poll_network_info();

        // Only poll blocks and transactions if not syncing (to avoid disrupting ycashd)
        if (!BlockIndexer::instance().is_syncing())
        {
            poll_latest_blocks();
            poll_latest_transactions();
        }
        else
        {
            logger.debug("Skipping block/transaction polling during sync");
        }

        // Wait for the configured interval before polling again
        // Sleep for total interval, but check stop_token every 100ms for responsive shutdown
        for (int i = 0; i < (config.zmq_polling_interval * 10) && !stop_token.stop_requested(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    logger.info("PollingFallback thread ended");
}

void PollingFallback::poll_network_info()
{
    try
    {
        YcashRpcClient client(config);
        InfoResponse info = client.get_info();

        // Serialize typed response back to JSON for compatibility with state management
        std::string json_str;
        auto ec = glz::write_json(info, json_str);
        if (ec)
        {
            Logger::instance().warn("Failed to parse network info JSON");
            return;
        }

        glz::generic network_data;
        ec = glz::read_json(network_data, json_str);
        if (ec)
        {
            Logger::instance().warn("Failed to parse network info JSON");
            return;
        }

        if (!network_data.empty() && !network_data.is_null())
        {
            // Update in-memory state
            BlockchainState::instance().set_proxy_network_info(network_data);
            // Broadcast to WebSocket clients
            EventBroadcaster::instance().broadcast_network_update(network_data);
            Logger::instance().debug("Network info updated in state and broadcasted from polling fallback");
        }
    }
    catch (const std::exception& e)
    {
        Logger::instance().errorf("PollingFallback poll_network_info error: {}", e.what());
    }
}

void PollingFallback::poll_latest_blocks()
{
    try
    {
        YcashRpcClient client(config);
        BlockCountResponse count_resp = client.get_block_count();
        int block_count = count_resp.count;
        glz::generic::array_t blocks_array;

        // Fetch last 10 blocks
        int start_height = std::max(0, block_count - 10);
        for (int i = block_count; i > start_height; --i)
        {
            BlockHashResponse hash_resp = client.get_block_hash(i);
            std::string block_hash = hash_resp.hash;
            YcashBlock block = client.get_block_by_hash(block_hash);

            // Serialize block to JSON for compatibility
            std::string json_str;
            auto ec = glz::write_json(block, json_str);
            if (ec) {
                Logger::instance().warn("Failed to serialize block to JSON");
                return;
            }        

            glz::generic block_result;
            ec = glz::read_json(block_result, json_str);
            if (!ec)
            {
                blocks_array.push_back(block_result);
            }
            else
            {
                Logger::instance().warn("Failed to push block to JSON array");
            }
        }

        if (blocks_array.size() > 0)
        {
            // Update in-memory state with each block
            for (const auto& block : blocks_array)
            {
                BlockchainState::instance().add_block(block);
            }
            // Broadcast to WebSocket clients
            EventBroadcaster::instance().broadcast_message("blocks", blocks_array);
            Logger::instance().debugf("Blocks updated in state and broadcasted from polling fallback ({} blocks)", blocks_array.size());
        }
    }
    catch (const std::exception& e)
    {
        Logger::instance().errorf("PollingFallback poll_latest_blocks error: {}", e.what());
    }
}

void PollingFallback::poll_latest_transactions()
{
    try
    {
        YcashRpcClient client(config);
        BlockCountResponse count_resp = client.get_block_count();
        int block_count = count_resp.count;
        glz::generic::array_t tx_array;

        // Fetch transaction hashes from last 10 blocks (don't try to fetch full tx details)
        int start_height = std::max(0, block_count - 10);
        for (int i = block_count; i > start_height && tx_array.size() < 20; --i)
        {
            BlockHashResponse hash_resp = client.get_block_hash(i);
            std::string block_hash = hash_resp.hash;
            YcashBlock block = client.get_block_by_hash(block_hash);

            if (!block.tx.empty())
            {
                // Just send transaction hashes instead of full details
                for (const auto& tx_hash : block.tx)
                {
                    if (tx_array.size() >= 20) break;

                    glz::generic tx_info;
                    tx_info["txid"] = tx_hash;
                    tx_info["block"] = block_hash;
                    tx_array.push_back(tx_info);
                }
            }
        }

        if (tx_array.size() > 0)
        {
            // Update in-memory state with each transaction
            for (const auto& tx : tx_array)
            {
                BlockchainState::instance().add_transaction(tx);
            }
            // Broadcast to WebSocket clients
            EventBroadcaster::instance().broadcast_message("transactions", tx_array);
            Logger::instance().debugf("Transactions updated in state and broadcasted from polling fallback ({} transactions)", tx_array.size());
        }
    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("PollingFallback poll_latest_transactions error: {}", e.what()));
    }
}
