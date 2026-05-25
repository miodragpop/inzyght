#pragma once

#include <thread>
#include <atomic>
#include "RpcConfig.h"

class PollingFallback
{
    public:
        // Singleton instance
        static PollingFallback& instance();

        // Initialize with RPC config
        bool initialize(const RpcConfig::Config& config);

        // Start polling fallback service
        void start();

        // Stop polling fallback service
        void stop();

        // Destructor
        ~PollingFallback();

    private:
        PollingFallback();

        // Polling thread function
        void polling_thread_func(std::stop_token stop_token);

        // Poll network info from RPC
        void poll_network_info();

        // Poll latest blocks from RPC
        void poll_latest_blocks();

        // Poll latest transactions from RPC
        void poll_latest_transactions();

        // Configuration
        RpcConfig::Config config;

        // Thread management
        std::jthread polling_thread;
        std::atomic<bool> active {false};
};
