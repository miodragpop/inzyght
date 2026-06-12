#include "TransactionController.h"
#include "glaze/json/json_concepts.hpp"
#include "services/BlockchainState.h"
#include "ycash_rpc_client.h"
#include "Logger.h"
#include "services/BlockIndexer.h"
#include "orm/PostgresPool.h"
#include "glaze/json/generic.hpp"
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <vector>


void TransactionController::get_latest_transactions(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    Logger& logger {Logger::instance()};
    glz::generic response;
    std::string response_str;
    response["data"] = glz::generic::array_t();

    auto send = [&](drogon::HttpStatusCode code) {
        glz::write_json(response, response_str);
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        resp->setBody(response_str);
        callback(resp);
    };

    try
    {
        if (BlockIndexer::instance().is_syncing())
        {
            logger.debug("Skipping get_latest_transactions API call during sync");
            response["status"] = "success";
            response["syncing"] = true;
            send(drogon::k200OK);
            return;
        }

        BlockchainState& current_state {BlockchainState::instance()};
        
        std::vector<std::string> mempool_txids {current_state.get_cached_mempool()};
        
        for (const std::string& one_mempool_txid : mempool_txids)
        {
            glz::generic tx_obj;
            tx_obj["hash"] = one_mempool_txid;
            tx_obj["blockheight"] = glz::json_null<int>;
            tx_obj["status"] = "mempool";
            response["data"].get_array().push_back(tx_obj);
        }

        QueryConnectionGuard db;

        const char* query =
            "SELECT encode(hash, 'hex'), block_height "
            "FROM transactions "
            "ORDER BY block_height DESC, tx_index ASC "
            "LIMIT 20";

        PGresult* res = PQexec(db.conn(), query);

        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            logger.errorf("Failed to query transactions: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"] = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i)
        {
            glz::generic tx_obj;
            tx_obj["hash"] = std::string(PQgetvalue(res, i, 0));
            tx_obj["blockheight"] = std::stoi(PQgetvalue(res, i, 1));
            tx_obj["status"] = "confirmed";
            response["data"].get_array().push_back(tx_obj);
        }

        PQclear(res);

        response["status"] = "success";
        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_latest_transactions: {}", e.what());
        response["status"] = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}


void TransactionController::get_all_transactions(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    Logger& logger {Logger::instance()};
    glz::generic response;
    std::string resp_body;

    auto send = [&](drogon::HttpStatusCode code) {
        glz::write_json(response, resp_body);
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        resp->setBody(resp_body);
        callback(resp);
    };

    try
    {
        const auto& params = req->getParameters();

        int draw   = 1;
        int start  = 0;
        int length = 25;
        try { draw   = std::stoi(params.at("draw"));   } catch (...) {}
        try { start  = std::stoi(params.at("start"));  } catch (...) {}
        try { length = std::stoi(params.at("length")); } catch (...) {}
        length = std::clamp(length, 1, 200);

        // Skip during catch-up sync: serving partial counts that change every
        // few seconds makes for a confusing UI, and the COUNT(*) avoidance
        // here is moot if the table is still being populated. Return an
        // empty DataTables-shaped response with syncing:true so the client
        // can display a "syncing in progress" notice.
        if (BlockIndexer::instance().is_syncing())
        {
            logger.debug("Skipping get_all_transactions API call during sync");
            response["draw"]            = draw;
            response["recordsTotal"]    = glz::generic_i64(0);
            response["recordsFiltered"] = glz::generic_i64(0);
            response["data"]            = glz::generic::array_t{};
            response["syncing"]         = true;
            send(drogon::k200OK);
            return;
        }

        // Total count — read from the cached getblockchaininfo response
        // instead of SELECT COUNT(*) FROM transactions (which would be a
        // multi-second sequential scan on the full table for every page
        // request). The cache is refreshed by EventProcessor on every block
        // event, so it's at most ~75s stale — fine for DataTables pagination
        // display.
        const int64_t total =
            BlockchainState::instance().get_cached_blockchain_info().transactions;

        QueryConnectionGuard db;

        // Paginated rows
        const std::string length_str = std::to_string(length);
        const std::string start_str  = std::to_string(start);
        const char* row_params[] = { length_str.c_str(), start_str.c_str() };
        PGresult* res = PQexecParams(db.conn(),
            "SELECT encode(hash, 'hex'), block_height, timestamp, is_coinbase "
            "FROM transactions "
            "ORDER BY block_height DESC, tx_index ASC "
            "LIMIT $1 OFFSET $2",
            2, nullptr, row_params, nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            logger.errorf("get_all_transactions query failed: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"]  = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        glz::generic::array_t rows_arr;
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            glz::generic::object_t tx;
            tx["hash"]        = std::string(PQgetvalue(res, i, 0));
            tx["height"]      = std::stoi(PQgetvalue(res, i, 1));
            tx["time"]        = std::stoll(PQgetvalue(res, i, 2));
            tx["is_coinbase"] = std::string(PQgetvalue(res, i, 3)) == "t";
            rows_arr.push_back(tx);
        }
        PQclear(res);

        response["draw"]            = draw;
        response["recordsTotal"]    = glz::generic_i64(total);
        response["recordsFiltered"] = glz::generic_i64(total);
        response["data"]            = std::move(rows_arr);

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_all_transactions: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}


void TransactionController::get_transaction_verbose(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
{
    Logger& logger {Logger::instance()};
    glz::generic::object_t response {};
    std::string resp_body {};

    auto send = [&](drogon::HttpStatusCode code) {
        glz::write_json(response, resp_body);
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        resp->setBody(resp_body);
        callback(resp);
    };

    // Optional blockhash: when navigating from an orphaned (off-chain) block,
    // the page passes ?blockhash= so the node can still locate a tx that plain
    // getrawtransaction can't find on the active chain (e.g. an orphaned
    // coinbase).
    const std::string block_hash = req->getParameter("blockhash");

    // Detect the node's off-chain signal: height = -1 and/or confirmations = -1.
    // A mempool tx has NO height (absent) and confirmations 0, so check the
    // optionals for a negative value rather than value_or(0) - otherwise
    // mempool would be misflagged.
    auto is_off_chain = [](const RawTransaction& t) {
        return (t.confirmations.has_value() && *t.confirmations < 0) ||
               (t.height.has_value()        && *t.height        < 0);
    };

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();

        // Always try the main chain first. A non-coinbase tx from an orphaned
        // block is usually re-mined into a later main-chain block under the
        // SAME txid, so it's perfectly valid - we must not mislabel it as
        // orphaned just because the request carried an orphan blockhash. Only
        // fall back to the supplied blockhash if the tx isn't on the main
        // chain (the genuine orphan case, e.g. an orphaned coinbase).
        RawTransaction tx;
        bool off_chain = false;
        try
        {
            tx = client.get_raw_transaction(hash);
            // An empty txid means the node didn't actually resolve it on the
            // main chain (ycashd may return an empty object instead of an
            // error), so treat that as "not on chain" too.
            off_chain = tx.txid.empty() || is_off_chain(tx);
        }
        catch (const std::exception&)
        {
            off_chain = true;  // not resolvable on the active chain
        }

        if (off_chain && !block_hash.empty())
        {
            // Not on the main chain - resolve the orphaned copy via its block.
            tx = client.get_raw_transaction(hash, block_hash);
            off_chain = is_off_chain(tx);
        }

        if (tx.txid.empty())
        {
            // Node returned nothing usable (can happen for a tx removed by a
            // reorg with no blockhash to fall back to).
            throw std::runtime_error("transaction not resolvable");
        }

        response["status"] = "success";
        auto& d = response["data"];

        d["txid"]          = tx.txid;
        d["version"]       = tx.version;
        d["locktime"]      = tx.locktime;
        d["size"]          = tx.size.value_or(0);
        d["expiry_height"] = tx.expiryheight.value_or(0);
        d["overwintered"]  = tx.overwintered.value_or(false);
        d["confirmations"] = tx.confirmations.value_or(0);
        d["off_chain"]     = off_chain;
        d["blockhash"]     = tx.blockhash.value_or(block_hash);
        d["blocktime"]     = tx.blocktime.value_or(0);
        d["height"]        = tx.height.value_or(0);
        d["value_balance"] = tx.valueBalance.value_or(0.0);

        // Inputs (kept as generic — already well-structured from RPC)
        d["vin"] = glz::generic::array_t {};
        for (const auto& v : tx.vin)
            d["vin"].get_array().push_back(v);

        // Outputs
        d["vout"] = glz::generic::array_t {};
        for (const auto& v : tx.vout)
            d["vout"].get_array().push_back(v);

        // Shielded counts
        d["sapling_spends"]  = static_cast<int>(tx.vShieldedSpend ? tx.vShieldedSpend->size() : 0);
        d["sapling_outputs"] = static_cast<int>(tx.vShieldedOutput ? tx.vShieldedOutput->size() : 0);

        // Sprout joinsplits (keep as generic)
        d["vjoinsplit"] = glz::generic::array_t {};
        if (tx.vjoinsplit)
            for (const auto& js : *tx.vjoinsplit)
                d["vjoinsplit"].get_array().push_back(js);

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.warnf("get_transaction_verbose failed for {}: {}", hash, e.what());
        response["status"] = "error";
        // A tx removed by a reorg rollback is no longer decodable here (and the
        // DB row is gone), so we can't distinguish "orphaned" from "never
        // existed" by txid alone. If the request carried a blockhash, the
        // caller already knows it came from an off-chain block - surface that.
        if (!block_hash.empty()) {
            response["off_chain"] = true;
            response["message"] = "Transaction is not on the main chain";
        } else {
            response["message"] = "Transaction not found";
        }
        send(drogon::k404NotFound);
    }
}

void TransactionController::get_transaction_raw(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
{
    Logger& logger {Logger::instance()};

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();
        std::string raw = client.get_transaction_raw_json(hash);
        auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
        resp->setBody(raw);
        callback(resp);
    }
    catch (const std::exception& e)
    {
        logger.warnf("get_transaction_raw failed for {}: {}", hash, e.what());
        std::string body = R"({"status":"error","message":"Transaction not found"})";
        auto resp = HttpResponse::newHttpResponse(drogon::k404NotFound, drogon::CT_APPLICATION_JSON);
        resp->setBody(body);
        callback(resp);
    }
}

void TransactionController::get_transaction_by_hash(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
{
    Logger& logger = Logger::instance();
    glz::generic response;
    std::string response_str;
    glz::error_ctx ec;

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();

        try
        {
            // Get transaction data using typed response
            RawTransaction tx = client.get_raw_transaction(hash);
            
            response["status"] = "success";
            response["data"]["hash"] = tx.txid;
            response["data"]["version"] = tx.version;
            response["data"]["locktime"] = tx.locktime;

            // Process inputs
            response["data"]["inputs"] = glz::generic::array_t();
            for (const auto& input : tx.vin)
            {
                response["data"]["inputs"].get_array().push_back(input);
            }

            // Process outputs
            response["data"]["outputs"] = glz::generic::array_t();
            for (const auto& output : tx.vout)
            {
                response["data"]["outputs"].get_array().push_back(output);
            }

            ec = glz::write_json(response, response_str);
            auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
            resp->setBody(response_str);
            callback(resp);
        }
        catch (const std::exception& e)
        {
            logger.warnf("Failed to get transaction: {}", hash);
            response["status"] = "error";
            response["message"] = "Transaction not found";
            
            ec = glz::write_json(response, response_str);
            auto resp = HttpResponse::newHttpResponse(drogon::k404NotFound, drogon::CT_APPLICATION_JSON);
            resp->setBody(response_str);
            callback(resp);
            return;
        }
    }
    catch (const std::exception& e)
    {
        Logger& logger = Logger::instance();
        logger.errorf("Exception in get_transaction_by_hash: {}", e.what());
        response["status"] = "error";
        response["message"] = "Server error";

        ec = glz::write_json(response, response_str);
        auto resp = HttpResponse::newHttpResponse(drogon::k500InternalServerError, drogon::CT_APPLICATION_JSON);
        resp->setBody(response_str);
        callback(resp);
    }
}


void TransactionController::get_mempool(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic response;
    std::string response_str;

    auto mempool = BlockchainState::instance().get_cached_mempool_full();

    glz::generic::array_t items;
    for (const auto& [txid, info] : mempool)
    {
        glz::generic entry;
        entry["txid"]     = txid;
        entry["size"]     = info.size;
        entry["fee"]      = info.fee;
        entry["time"]     = info.time;
        entry["height"]   = info.height;
        entry["depends"]  = info.depends.size();
        items.push_back(std::move(entry));
    }

    response["status"] = "success";
    response["data"]   = std::move(items);
    response["count"]  = static_cast<int64_t>(mempool.size());
    // Freshness of the underlying snapshot: when ycashd stops answering RPC
    // the snapshot is kept (not pruned), so surface its age instead of
    // presenting frozen data as live.
    response["as_of_age_seconds"] = glz::generic_i64(BlockchainState::instance().get_snapshot_age_seconds());
    response["stale"]  = BlockchainState::instance().is_snapshot_stale();

    glz::write_json(response, response_str);
    auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
    resp->setBody(response_str);
    callback(resp);
}
