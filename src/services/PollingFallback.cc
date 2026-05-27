#include "PollingFallback.h"
#include "ZeroMQListener.h"
#include "BlockIndexer.h"
#include "ycash_rpc_client.h"
#include "Logger.h"
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

void PollingFallback::start()
{
    if (active)
    {
        return;
    }

    Logger::instance().info("Starting PollingFallback service");
    active = true;
    polling_thread = std::jthread(&PollingFallback::polling_thread_func, this);
}

void PollingFallback::stop()
{
    if (!active)
    {
        return;
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
        if (!ZeroMQListener::instance().is_in_fallback_mode())
        {
            // ZMQ is healthy — sleep 30s in 100ms slices for responsive shutdown.
            for (int i = 0; i < 300 && !stop_token.stop_requested(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Skip during initial sync — BlockIndexer is already hammering ycashd
        // and EventProcessor would skip the broadcast anyway.
        if (!BlockIndexer::instance().is_syncing())
        {
            std::string tip = fetch_tip_hash();
            if (!tip.empty() && tip != last_tip_hash)
            {
                logger.debugf("PollingFallback: new tip detected ({}), injecting block event", tip);
                ZeroMQListener::instance().inject_event("block", tip);
                last_tip_hash = tip;
            }
        }

        // Sleep zmq_polling_interval seconds in 100ms slices.
        for (int i = 0; i < (config.zmq_polling_interval * 10) && !stop_token.stop_requested(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    logger.info("PollingFallback thread ended");
}

std::string PollingFallback::fetch_tip_hash()
{
    try
    {
        YcashRpcClient& client = YcashRpcClient::instance();
        BlockCountResponse count_resp = client.get_block_count();
        BlockHashResponse hash_resp = client.get_block_hash(count_resp.count);
        return hash_resp.hash;
    }
    catch (const std::exception& e)
    {
        Logger::instance().errorf("PollingFallback fetch_tip_hash error: {}", e.what());
        return {};
    }
}
