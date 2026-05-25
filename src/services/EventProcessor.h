#pragma once

#include <thread>
#include <atomic>
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
        void process_transaction_event(const std::string& tx_hash);

        RpcConfig::Config config;
        std::jthread processor_thread;
        std::atomic<bool> running {false};
};
