#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "RpcConfig.h"

// When ZeroMQ goes silent, PollingFallback polls ycashd for the current tip
// and synthesizes a "block" event into the same queue ZeroMQListener feeds.
// EventProcessor then processes it identically to a real ZMQ event, so cache
// refresh and broadcast semantics stay uniform across both paths.
class PollingFallback
{
    public:
        static PollingFallback& instance();

        bool initialize(const RpcConfig::Config& config);
        void start();
        void stop();

        ~PollingFallback();

    private:
        PollingFallback();

        void polling_thread_func(std::stop_token stop_token);

        // Returns the current tip hash, or empty string on RPC failure.
        std::string fetch_tip_hash();

        RpcConfig::Config config;

        std::jthread polling_thread;
        std::atomic<bool> active {false};

        // Last tip hash we synthesized an event for, so we don't re-emit on
        // every tick when the chain is idle.
        std::string last_tip_hash;
};
