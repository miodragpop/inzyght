#pragma once

#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include "RpcConfig.h"

class EventProcessor
{
    public:
        static EventProcessor& instance();

        bool initialize(const RpcConfig::Config& conf);
        void start();
        void stop();

    private:
        EventProcessor() = default;
        ~EventProcessor();

        void processor_thread_func(std::stop_token stop_token);
        void process_block_event(const std::string& block_hash);
        // Handles a whole drained batch of transaction events with a single
        // mempool refresh (see processor_thread_func for why they're batched).
        void process_transaction_events(const std::vector<std::string>& tx_hashes);

        RpcConfig::Config config;
        std::jthread processor_thread;
        std::atomic<bool> running {false};
};
