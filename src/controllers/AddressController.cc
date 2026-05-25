#include "AddressController.h"
#include "ycash_rpc_client.h"
#include "ConfigManager.h"
#include "RpcConfig.h"
#include "Logger.h"
#include "services/BlockchainState.h"
#include "orm/PostgresPool.h"
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <algorithm>
#include <format>
#include <sstream>
#include <unordered_map>
#include "glaze/json/generic.hpp"

// ── Chunked delta fetching constants ─────────────────────────────────────────
// Scan 50 000 blocks per getaddressdeltas call (≈ 43 days of Ycash blocks).
// Stop early once we have enough unique txids for a comfortable first page.
// Allow up to MAX_CHUNKS calls per request so we don't stall forever on
// addresses with very sparse activity.
static constexpr int CHUNK_BLOCKS  = 50'000;
static constexpr int MIN_TX_TARGET = 50;    // stop when we have this many txids
static constexpr int MAX_CHUNKS    = 6;     // max RPC calls per request (≈ 260k blocks)

void AddressController::get_address_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& address) const
{
    Logger& logger {Logger::instance()};
    ConfigManager& config {ConfigManager::instance()};
    glz::generic::object_t response {};
    std::string resp_body {};

    auto send = [&](drogon::HttpStatusCode code) {
        glz::write_json(response, resp_body);
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        resp->setBody(resp_body);
        callback(resp);
    };

    try
    {
        YcashRpcClient client(config.get_rpc_config());

        // ── Parse offset_height query param ───────────────────────────────────
        // First call: no param → use chain tip.
        // Subsequent "load more" calls: frontend sends the next_offset_height
        // returned by the previous response.
        int offset_height = 0;
        const auto& params = req->getParameters();
        auto it = params.find("offset_height");
        if (it != params.end()) {
            try { offset_height = std::stoi(it->second); } catch (...) {}
        }
        const bool is_first_call = (offset_height == 0);

        if (offset_height <= 0) {
            offset_height = BlockchainState::instance().get_cached_info().blocks;
        }

        // ── First/last active height (only on first call) ─────────────────────
        // Bounds the delta scan so we don't walk the whole chain for inactive addresses.
        int first_height = 1;
        if (is_first_call) {
            try {
                auto [fh, lh] = client.get_address_first_last_height(address);
                if (lh > 0) offset_height = lh;  // start scan from last seen block
                if (fh > 0) first_height  = fh;  // stop scan when we pass first seen block
            } catch (const std::exception& e) {
                logger.warnf("get_address_first_last_height for {}: {}", address, e.what());
            }
        }

        // ── Balance (only on first call — one fast RPC) ───────────────────────
        AddressBalance bal {};
        if (is_first_call) {
            bal = client.get_address_balance_single(address);
        }

        // ── Chunked delta fetch ───────────────────────────────────────────────
        // Scan chunks of CHUNK_BLOCKS starting at offset_height, going backwards.
        // Stop at first_height (no activity below it), MIN_TX_TARGET txids, or MAX_CHUNKS.
        struct TxSummary { int64_t net_sat{0}; int height{0}; bool is_coinbase{false}; };
        std::unordered_map<std::string, TxSummary> tx_map;
        std::vector<std::string> tx_order;

        int scan_top = offset_height;
        int chunks   = 0;

        while (scan_top >= first_height && (int)tx_order.size() < MIN_TX_TARGET && chunks < MAX_CHUNKS) {
            int from = std::max(first_height, scan_top - CHUNK_BLOCKS + 1);
            int to   = scan_top;

            try {
                auto chunk = client.get_address_deltas_single(address, from, to);
                for (const auto& d : chunk) {
                    if (tx_map.find(d.txid) == tx_map.end()) {
                        tx_map[d.txid] = {0, d.height};
                        tx_order.push_back(d.txid);
                    }
                    tx_map[d.txid].net_sat += d.satoshis;
                }
            } catch (const std::exception& e) {
                logger.warnf("get_address_deltas_single [{},{}] for {}: {}",
                             from, to, address, e.what());
            }

            scan_top = from - 1;
            ++chunks;
        }

        // Sort by height descending (most recent first)
        std::sort(tx_order.begin(), tx_order.end(), [&](const std::string& a, const std::string& b){
            return tx_map[a].height > tx_map[b].height;
        });

        const bool has_more           = (scan_top >= first_height);
        const int  next_offset_height = has_more ? scan_top : -1;

        // ── Block timestamps + coinbase flag for found transactions ──────────────
        // Collect unique heights for timestamp, and txids for is_coinbase — one DB
        // round-trip each.
        std::unordered_map<int, int64_t> height_to_time;
        if (!tx_order.empty()) {
            std::ostringstream height_arr;
            height_arr << "ARRAY[";
            bool first_h = true;
            for (const auto& txid : tx_order) {
                if (!first_h) height_arr << ',';
                height_arr << tx_map[txid].height;
                first_h = false;
            }
            height_arr << "]";
            std::string ts_query = std::format(
                "SELECT height, timestamp FROM blocks WHERE height = ANY({})", height_arr.str());

            std::ostringstream txid_list;
            bool first_t = true;
            for (const auto& txid : tx_order) {
                if (!first_t) txid_list << ',';
                txid_list << R"(\x)" << txid;
                first_t = false;
            }
            // Use bytea literals so PostgreSQL can use the hash index (no full table scan)
            std::string cb_query = std::format(
                "SELECT encode(hash,'hex'), is_coinbase FROM transactions WHERE hash = ANY(ARRAY[{}]::bytea[])", txid_list.str());

            try {
                QueryConnectionGuard db;
                PGresult* ts_res = PQexec(db.conn(), ts_query.c_str());
                if (PQresultStatus(ts_res) == PGRES_TUPLES_OK) {
                    int rows = PQntuples(ts_res);
                    for (int i = 0; i < rows; ++i) {
                        int     h  = std::stoi(PQgetvalue(ts_res, i, 0));
                        int64_t ts = std::stoll(PQgetvalue(ts_res, i, 1));
                        height_to_time[h] = ts;
                    }
                }
                PQclear(ts_res);

                PGresult* cb_res = PQexec(db.conn(), cb_query.c_str());
                if (PQresultStatus(cb_res) == PGRES_TUPLES_OK) {
                    int rows = PQntuples(cb_res);
                    for (int i = 0; i < rows; ++i) {
                        std::string txid = PQgetvalue(cb_res, i, 0);
                        bool is_cb = std::string(PQgetvalue(cb_res, i, 1)) == "t";
                        if (tx_map.count(txid)) tx_map[txid].is_coinbase = is_cb;
                    }
                }
                PQclear(cb_res);
            } catch (const std::exception& e) {
                logger.warnf("Block timestamp/coinbase lookup failed: {}", e.what());
            }
        }

        // ── Mempool (only on first call) ──────────────────────────────────────
        std::unordered_map<std::string, int64_t> mp_map;
        std::vector<std::string> mp_order;
        if (is_first_call) {
            try {
                auto mp = client.get_address_mempool_single(address);
                for (const auto& m : mp) {
                    if (mp_map.find(m.txid) == mp_map.end()) {
                        mp_map[m.txid] = 0;
                        mp_order.push_back(m.txid);
                    }
                    mp_map[m.txid] += m.satoshis;
                }
            } catch (const std::exception& e) {
                logger.debugf("get_address_mempool_single for {}: {}", address, e.what());
            }
        }

        // ── Build response ────────────────────────────────────────────────────
        response["status"] = "success";
        auto& d = response["data"];
        d["address"]            = address;
        d["has_more"]           = has_more;
        d["next_offset_height"] = next_offset_height;

        d["chain_height"] = BlockchainState::instance().get_cached_info().blocks;

        if (is_first_call) {
            int64_t sent_sat = bal.received - bal.balance;
            d["balance"]   = glz::generic_i64(bal.balance);
            d["received"]  = glz::generic_i64(bal.received);
            d["sent"]      = glz::generic_i64(sent_sat);
            d["tx_count"]  = static_cast<int>(bal.txcount);

            d["mempool"] = glz::generic::array_t{};
            for (const auto& txid : mp_order) {
                glz::generic entry;
                entry["txid"]       = txid;
                entry["net_change"] = glz::generic_i64(mp_map[txid]);
                d["mempool"].get_array().push_back(std::move(entry));
            }
        }

        d["transactions"] = glz::generic::array_t{};
        for (const auto& txid : tx_order) {
            const auto& s = tx_map[txid];
            glz::generic entry;
            entry["txid"]        = txid;
            entry["height"]      = s.height;
            entry["net_change"]  = glz::generic_i64(s.net_sat);
            entry["is_coinbase"] = s.is_coinbase;
            auto ts_it = height_to_time.find(s.height);
            entry["time"] = glz::generic_i64(ts_it != height_to_time.end() ? ts_it->second : 0);
            d["transactions"].get_array().push_back(std::move(entry));
        }

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.warnf("get_address_info failed for {}: {}", address, e.what());
        response["status"]  = "error";
        response["message"] = "Address not found or node error";
        send(drogon::k404NotFound);
    }
}

void AddressController::get_address_transactions(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& address) const
{
    try
    {
        Logger& logger = Logger::instance();
        ConfigManager& config = ConfigManager::instance();

        YcashRpcClient client(config.get_rpc_config());

        try
        {
            // Get address transactions using typed response
            TransactionInfoListResponse tx_result; // = client.list_transactions(address);

            glz::generic response;
            std::string response_str;

            response["status"] = "success";
            response["data"] = glz::generic::array_t();

            // Process each transaction
            for (const auto& tx : tx_result.result)
            {
                glz::generic tx_info;
                tx_info["hash"] = tx.txid;
                tx_info["time"] = tx.time;
                tx_info["amount"] = tx.amount;
                tx_info["category"] = tx.category;
                tx_info["confirmations"] = tx.confirmations;
                tx_info["blockhash"] = tx.blockhash;
                tx_info["blockindex"] = tx.blockindex;
                tx_info["blocktime"] = tx.blocktime;
                tx_info["status"] = (tx.confirmations > 0) ? "confirmed" : "pending";

                response["data"].get_array().push_back(tx_info);
            }

            auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
            auto ec = glz::write_json(response, response_str);
            resp->setBody(response_str);
            callback(resp);
        }
        catch (const std::exception& e)
        {
            logger.warnf("Failed to get transactions for address: {}", address);
            glz::generic response;
            std::string response_str;

            response["status"] = "success";
            response["data"] = glz::generic::array_t();
            auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
            auto ec = glz::write_json(response, response_str);
            resp->setBody(response_str);
            callback(resp);
            return;
        }

    }
    catch (const std::exception& e)
    {
        Logger& logger = Logger::instance();
        logger.errorf("Exception in get_address_transactions: {}", e.what());
        glz::generic response;
        std::string response_str;
        response["status"] = "error";
        response["message"] = "Server error";
        auto resp = HttpResponse::newHttpResponse(drogon::k500InternalServerError, drogon::CT_APPLICATION_JSON);
        auto ec = glz::write_json(response, response_str);
        
        auto test = response.dump();
        
        resp->setBody(response_str);
        
        callback(resp);
    }
}

