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

void BlockchainState::add_block(const glz::generic& block)
{
    std::scoped_lock<std::mutex> lock(state_mutex);

    // Check if block already exists (by hash)
    if (block.contains("hash"))
    {
        std::string block_hash = block.at("hash").get_string();
        for (const glz::generic& existing_block : proxy_latest_blocks)
        {
            if (existing_block.contains("hash") && existing_block["hash"].get_string() == block_hash)
            {
                // Block already exists, update it
                proxy_latest_blocks.erase(
                    std::remove_if(proxy_latest_blocks.begin(),
                                   proxy_latest_blocks.end(),
                                   [&block_hash](const glz::generic& b) {
                                        return b.contains("hash") && b["hash"].get_string() == block_hash;
                                   }),
                                   proxy_latest_blocks.end());
                break;
            }
        }
    }

    // Add block to front (newest first)
    proxy_latest_blocks.insert(proxy_latest_blocks.begin(), block);

    // Keep only latest N blocks
    if (proxy_latest_blocks.size() > MAX_BLOCKS)
    {
        proxy_latest_blocks.erase(proxy_latest_blocks.begin() + MAX_BLOCKS, proxy_latest_blocks.end());
    }

    Logger::instance().debugf("Block added to state, total blocks: {}", proxy_latest_blocks.size());
}


void BlockchainState::add_transaction(const glz::generic& tx)
{
    std::scoped_lock<std::mutex> lock(state_mutex);

    // Check if transaction already exists (by txid)
    if (tx.contains("txid"))
    {
        std::string txid = tx["txid"].get_string();

        for (const auto& existing_tx : proxy_latest_transactions)
        {
            if (existing_tx.contains("txid") && existing_tx["txid"].get_string() == txid)
            {
                // Transaction already exists, update it
                proxy_latest_transactions.erase(std::remove_if(proxy_latest_transactions.begin(),
                                                               proxy_latest_transactions.end(),
                                                               [&txid](const glz::generic& t) {
                                                                   return t.contains("txid") && t["txid"].get_string() == txid;
                                                               }),
                                                               proxy_latest_transactions.end());
                break;
            }
        }
    }

    // Add transaction to front (newest first)
    proxy_latest_transactions.insert(proxy_latest_transactions.begin(), tx);

    // Keep only latest N transactions
    if (proxy_latest_transactions.size() > MAX_TRANSACTIONS)
    {
        proxy_latest_transactions.erase(proxy_latest_transactions.begin() + MAX_TRANSACTIONS, proxy_latest_transactions.end());
    }

    Logger::instance().debugf("Transaction added to state, total transactions: {}", proxy_latest_transactions.size());
}


glz::generic BlockchainState::get_latest_transactions(int limit) const
{
    glz::generic tx_array;
    int count = 0;    
    
    {        
        std::scoped_lock<std::mutex> lock(state_mutex);

        for (const auto& tx : proxy_latest_transactions)
        {
            if (count >= limit) break;
            tx_array.get_array().push_back(tx);
            count++;
        }
    }

    return tx_array;
}

glz::generic BlockchainState::get_latest_blocks(int limit) const
{
    glz::generic block_array;
    int count = 0;    

    {
        std::scoped_lock<std::mutex> lock(state_mutex);

        for (const auto& block : proxy_latest_blocks)
        {
            if (count >= limit) break;
            block_array.get_array().push_back(block);
            count++;
        }
    }

    return block_array;
}

glz::generic BlockchainState::get_current_proxy_state() const
{
    std::scoped_lock<std::mutex> lock(state_mutex);

    glz::generic state {};
    state["network"] = proxy_network_info;
    state["blocks"] = proxy_latest_blocks;
    state["transactions"] = proxy_latest_transactions;

    return state;
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
        parse_one(2, kMethodNames[2], cached_peer_info_response);
        parse_one(3, kMethodNames[3], cached_mining_info_response);
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