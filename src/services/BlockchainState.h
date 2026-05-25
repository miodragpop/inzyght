#pragma once

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

        void add_block(const glz::generic& block);
        void add_transaction(const glz::generic& tx);

        glz::generic get_latest_blocks(int limit = 20) const;
        glz::generic get_latest_transactions(int limit = 20) const;

        glz::generic get_current_proxy_state() const;

        void update_state_on_block_event();
        void update_state_on_transaction_event();

    private:
        BlockchainState() = default;
        ~BlockchainState() = default;

        mutable std::mutex state_mutex;

        BlockchainInfoResponse cached_blockchain_info_response {};
        NetworkInfoResponse cached_network_info_response {};
        std::vector<PeerInfoResponse> cached_peer_info_response {};
        MiningInfoResponse cached_mining_info_response {};
        std::map<std::string, MempoolTransactionInfo> cached_raw_mempool_response {};

        glz::generic proxy_network_info;
        std::vector<glz::generic> proxy_latest_blocks;
        std::vector<glz::generic> proxy_latest_transactions;

        static const int MAX_BLOCKS = 100;
        static const int MAX_TRANSACTIONS = 100;
};
