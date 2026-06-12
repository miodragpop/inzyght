#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include <glaze/json/generic.hpp>
#include "models/RpcResponses.h"

class BlockchainState
{
    public:
        static BlockchainState& instance();

        void set_proxy_network_info(const glz::generic& info);
        glz::generic get_proxy_network_info () const;

        InfoResponse get_cached_info() const;
        InfoResponseExtended get_cached_info_extended() const;
        BlockchainInfoResponse get_cached_blockchain_info() const;
        std::vector<std::string> get_cached_mempool() const;
        std::map<std::string, MempoolTransactionInfo> get_cached_mempool_full() const;
        size_t get_mempool_count() const;
        std::vector<PeerInfoResponse> get_cached_peers() const;

        // Both return true only when the node answered and the snapshot was
        // fully replaced; on failure the previous snapshot is kept and the
        // freshness timestamp is NOT advanced, so staleness becomes visible.
        bool update_state_on_block_event();
        bool update_state_on_transaction_event();

        // Seconds since the last successful refresh (or freshness
        // confirmation), or -1 if none succeeded yet. Lets API consumers
        // distinguish "live snapshot" from "ycashd stopped answering and
        // this data is frozen".
        long get_snapshot_age_seconds() const;

        // Stale = a refresh/probe provably failed (last_refresh_failed), or
        // freshness couldn't be verified for kStaleAfterSeconds. With the
        // EventProcessor heartbeat confirming quiet periods every ~60s, age
        // beyond 150s means even the heartbeat stopped getting through.
        static constexpr long kStaleAfterSeconds = 150;
        bool is_snapshot_stale() const;

        // Heartbeat signals from EventProcessor: a successful probe with an
        // unchanged tip proves the snapshot still matches reality without a
        // full refresh; a failed probe proves the node is unreachable.
        void confirm_snapshot_fresh();
        void mark_node_unreachable();

    private:
        BlockchainState() = default;
        ~BlockchainState() = default;

        mutable std::mutex state_mutex;

        BlockchainInfoResponse cached_blockchain_info_response {};
        NetworkInfoResponse cached_network_info_response {};
        std::vector<PeerInfoResponse> cached_peer_info_response {};
        MiningInfoResponse cached_mining_info_response {};
        std::map<std::string, MempoolTransactionInfo> cached_raw_mempool_response {};

        // Default-constructed = "never refreshed".
        std::chrono::steady_clock::time_point last_successful_refresh {};
        // True after a provably failed refresh or heartbeat probe; cleared by
        // the next success. Makes real outages surface immediately instead of
        // waiting out the age threshold.
        bool last_refresh_failed {false};

        glz::generic proxy_network_info;
};
