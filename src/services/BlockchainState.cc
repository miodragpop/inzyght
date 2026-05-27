#include "BlockchainState.h"
#include "Logger.h"
#include "glaze/json/generic.hpp"
#include "glaze/json/read.hpp"
#include "models/RpcResponses.h"
#include "ycash_rpc_client.h"
#include <algorithm>
#include <array>
#include <mutex>

BlockchainState& BlockchainState::instance()
{
    static BlockchainState instance;
    return instance;
}


void BlockchainState::set_proxy_network_info(const glz::generic& info)
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    proxy_network_info = info;
    Logger::instance().debug("Network info updated in state");
}


glz::generic BlockchainState::get_proxy_network_info() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    return proxy_network_info;
}

InfoResponse BlockchainState::get_cached_info() const
{
    InfoResponse info {};

    {
        std::scoped_lock<std::mutex> lock(state_mutex);
        info.version = cached_network_info_response.version;
        info.protocolversion = cached_network_info_response.protocolversion;
        info.blocks = cached_blockchain_info_response.blocks;
        info.connections = cached_network_info_response.connections;
        info.difficulty = cached_mining_info_response.difficulty;
        info.testnet = cached_mining_info_response.testnet;
        info.subversion = cached_network_info_response.subversion;
    }

    return info;
}

InfoResponseExtended BlockchainState::get_cached_info_extended() const
{
    InfoResponseExtended extended_info {};

    {
        std::scoped_lock<std::mutex> lock(state_mutex);
        extended_info.version = cached_network_info_response.version;
        extended_info.protocolversion = cached_network_info_response.protocolversion;
        extended_info.blocks = cached_blockchain_info_response.blocks;
        extended_info.connections = cached_network_info_response.connections;
        extended_info.difficulty = cached_mining_info_response.difficulty;
        extended_info.testnet = cached_mining_info_response.testnet;
        extended_info.subversion = cached_network_info_response.subversion;
        extended_info.total_transactions = cached_blockchain_info_response.transactions;
        extended_info.chain_supply = cached_blockchain_info_response.chainSupply.chainValue;
        extended_info.transparent_supply = cached_blockchain_info_response.valuePools[0].chainValue;
        extended_info.sprout_supply = cached_blockchain_info_response.valuePools[1].chainValue;
        extended_info.sapling_supply = cached_blockchain_info_response.valuePools[2].chainValue;
        extended_info.networksolps = cached_mining_info_response.networksolps;
    }

    return extended_info;
}

BlockchainInfoResponse BlockchainState::get_cached_blockchain_info() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    return cached_blockchain_info_response;
}

std::vector<std::string> BlockchainState::get_cached_mempool() const
{
    std::vector<std::string> mempool_txes {};

    for (const auto& mempool_entry : cached_raw_mempool_response)
    {
        mempool_txes.emplace_back(mempool_entry.first);
    }

    return mempool_txes;
}

std::map<std::string, MempoolTransactionInfo> BlockchainState::get_cached_mempool_full() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    return cached_raw_mempool_response;
}

size_t BlockchainState::get_mempool_count() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    return cached_raw_mempool_response.size();
}

std::vector<PeerInfoResponse> BlockchainState::get_cached_peers() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);
    return cached_peer_info_response;
}

void BlockchainState::update_state_on_block_event()
{
    Logger& logger = Logger::instance();
    YcashRpcClient& rpc_client = YcashRpcClient::instance();

    static constexpr std::array<const char*, 5> kMethodNames {
        "getblockchaininfo", "getnetworkinfo", "getpeerinfo", "getmininginfo", "getrawmempool"
    };

    std::vector<RpcRequest> multi_request {
        {1, kMethodNames[0], {}},
        {2, kMethodNames[1], {}},
        {3, kMethodNames[2], {}},
        {4, kMethodNames[3], {}},
        {5, kMethodNames[4], {true}}
    };

    std::string multi_response {};
    rpc_client.make_multi_json_rpc_request(multi_request, multi_response, YcashRpcClient::Backend::State);

    if (multi_response.empty())
    {
        logger.error("BlockchainState: empty response from multi-RPC; cache not updated (ycashd unreachable?)");
        return;
    }

    std::vector<JsonRpcResponse<glz::raw_json>> responses;
    auto envelope_ec = glz::read_json(responses, multi_response);
    if (envelope_ec)
    {
        logger.errorf("BlockchainState: failed to parse multi-RPC envelope: {}",
                      glz::format_error(envelope_ec, multi_response));
        return;
    }
    if (responses.size() != kMethodNames.size())
    {
        logger.errorf("BlockchainState: multi-RPC returned {} responses, expected {}",
                      responses.size(), kMethodNames.size());
        return;
    }

    // Each response must have a "result" — if "error" is set instead, the inner
    // RPC failed (e.g. ycashd module disabled) and result is std::nullopt.
    int parsed = 0;
    int failed = 0;
    {
        std::scoped_lock<std::mutex> lock(state_mutex);
        constexpr auto lenient = glz::opts{.error_on_unknown_keys = false};

        auto parse_one = [&](size_t idx, const char* method, auto& dest) {
            const auto& rpc_resp = responses[idx];
            if (!rpc_resp.result.has_value())
            {
                logger.errorf("BlockchainState: RPC '{}' returned no result (error: {})",
                              method, rpc_resp.error.value_or("unknown"));
                ++failed;
                return;
            }
            auto ec = glz::read<lenient>(dest, rpc_resp.result->str);
            if (ec)
            {
                logger.errorf("BlockchainState: failed to parse '{}' response: {}",
                              method, glz::format_error(ec, rpc_resp.result->str));
                ++failed;
                return;
            }
            ++parsed;
        };

        parse_one(0, kMethodNames[0], cached_blockchain_info_response);
        parse_one(1, kMethodNames[1], cached_network_info_response);
        // getpeerinfo returns an array — glaze appends to existing vector, so clear first.
        cached_peer_info_response.clear();
        parse_one(2, kMethodNames[2], cached_peer_info_response);
        parse_one(3, kMethodNames[3], cached_mining_info_response);
        // getrawmempool returns an object {txid: info}. Glaze parses into an
        // existing std::map by inserting/updating, NOT replacing — so stale
        // txids from prior calls would never get evicted and the count would
        // only ever grow. Clear before parsing so removed txids actually
        // leave the cache.
        cached_raw_mempool_response.clear();
        parse_one(4, kMethodNames[4], cached_raw_mempool_response);
    }

    if (failed > 0)
    {
        logger.warnf("BlockchainState: cache partially refreshed — {} ok, {} failed", parsed, failed);
    }
    else
    {
        logger.debugf("BlockchainState: cache refreshed ({} responses)", parsed);
    }
}


void BlockchainState::update_state_on_transaction_event()
{
    YcashRpcClient& rpc_client = YcashRpcClient::instance();
    std::string response_str = rpc_client.make_json_rpc_request("getrawmempool", {true}, YcashRpcClient::Backend::State);
    std::map<std::string, MempoolTransactionInfo> response;

    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(response, response_str);
    if (ec)
    {
        Logger::instance().errorf("Error reading JSON response in update_state_on_transaction_event: {}", ec.custom_error_message);
        return;
    }

    {
        std::scoped_lock<std::mutex> lock(state_mutex);
        cached_raw_mempool_response.swap(response);
    }
}