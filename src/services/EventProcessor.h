#pragma once

#include <thread>
#include <atomic>
#include <chrono>
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
        // Re-attempts a failed BlockchainState refresh between events, so the
        // snapshot self-heals as soon as ycashd answers again instead of
        // staying frozen until the next block event.
        void retry_state_refresh_if_needed();
        // During quiet stretches (no events for kHeartbeatAfter), probes the
        // node with one getbestblockhash: confirms snapshot freshness when
        // the tip is unchanged (so quiet != stale), refreshes when the tip
        // moved (missed event), and proves unreachability when it fails.
        void heartbeat_if_quiet();
        // Builds the extended network info from cached state and broadcasts
        // it to WebSocket clients (no RPC involved).
        void broadcast_network_info_from_cache();
        // After state recovers without any event having fired (outage healed
        // or heartbeat caught a missed block), frontends still show
        // pre-outage data and nothing nudges them until the next real event —
        // replay the broadcasts a block event would have produced.
        void broadcast_recovery_update();

        RpcConfig::Config config;
        std::jthread processor_thread;
        std::atomic<bool> running {false};

        // Touched only from the processor thread — no locking needed.
        bool state_refresh_needed {false};
        std::chrono::steady_clock::time_point last_refresh_retry {};
        static constexpr std::chrono::seconds kRefreshRetryInterval {10};
        static constexpr long kHeartbeatAfterSeconds {60};
};
