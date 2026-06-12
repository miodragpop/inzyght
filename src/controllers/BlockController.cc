#include "BlockController.h"
#include "glaze/json/generic.hpp"
#include "ycash_rpc_client.h"
#include "Logger.h"
#include "services/BlockchainState.h"
#include "services/BlockIndexer.h"
#include "orm/PostgresPool.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>


void BlockController::get_latest_blocks(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    Logger& logger = Logger::instance();
    logger.debugf("get_latest_blocks called from {}", req->getPeerAddr().toIp());

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
        const bool datatable_mode = (params.find("draw") != params.end());

        int draw   = 1;
        int start  = 0;
        int length = 10;

        if (datatable_mode) {
            try { draw   = std::stoi(params.at("draw"));   } catch (...) {}
            try { start  = std::stoi(params.at("start"));  } catch (...) {}
            try { length = std::stoi(params.at("length")); } catch (...) {}
            length = std::clamp(length, 1, 200);
        }

        if (BlockIndexer::instance().is_syncing())
        {
            logger.debug("Skipping get_latest_blocks API call during sync");
            if (datatable_mode) {
                // DataTables requires draw/recordsTotal/recordsFiltered fields
                // or its client treats the response as malformed.
                response["draw"]            = draw;
                response["recordsTotal"]    = glz::generic_i64(0);
                response["recordsFiltered"] = glz::generic_i64(0);
                response["data"]            = glz::generic::array_t{};
                response["syncing"]         = true;
            } else {
                response["status"]  = "success";
                response["syncing"] = true;
                response["data"]    = glz::generic::array_t{};
            }
            send(drogon::k200OK);
            return;
        }

        QueryConnectionGuard db;

        // ── Total count (for DataTables pagination display) ───────────────────
        // Read from the in-memory cache instead of SELECT COUNT(*) FROM blocks
        // (which would scan the whole table — hundreds of ms per request).
        // chain_height == count, since heights are dense from 1.
        int64_t total_blocks = 0;
        if (datatable_mode) {
            total_blocks = BlockchainState::instance().get_cached_info().blocks;
        }

        // ── Block rows ────────────────────────────────────────────────────────
        const std::string length_str = std::to_string(length);
        const std::string start_str  = std::to_string(start);
        const char* row_params[] = { length_str.c_str(), start_str.c_str() };
        PGresult* res = PQexecParams(db.conn(),
            "SELECT height, encode(hash, 'hex'), timestamp, tx_count, miner_address, size, claimed_reward_block "
            "FROM blocks "
            "ORDER BY height DESC "
            "LIMIT $1 OFFSET $2",
            2, nullptr, row_params, nullptr, nullptr, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            logger.errorf("Failed to query blocks: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"]  = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        glz::generic::array_t rows_arr;
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i)
        {
            glz::generic::object_t block;
            block["height"] = std::stoi(PQgetvalue(res, i, 0));
            block["hash"]   = std::string(PQgetvalue(res, i, 1));
            block["time"]   = std::stoll(PQgetvalue(res, i, 2));
            block["tx"]     = std::stoi(PQgetvalue(res, i, 3));
            block["miner"]  = std::string(PQgetvalue(res, i, 4));
            block["size"]   = std::stoi(PQgetvalue(res, i, 5));
            block["reward"] = std::stod(PQgetvalue(res, i, 6)) / 100000000.0;
            rows_arr.push_back(block);
        }
        PQclear(res);

        if (datatable_mode) {
            // DataTables server-side format
            response["draw"]            = draw;
            response["recordsTotal"]    = glz::generic_i64(total_blocks);
            response["recordsFiltered"] = glz::generic_i64(total_blocks);
            response["data"]            = std::move(rows_arr);
        } else {
            // Legacy format (main page)
            response["status"] = "success";
            response["data"]   = std::move(rows_arr);
        }

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_latest_blocks: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}



void BlockController::get_block_by_height(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, int height) const
{
    std::string resp_body {};

    try
    {
        Logger& logger = Logger::instance();

        YcashRpcClient& client = YcashRpcClient::thread_instance();

        try
        {
            // Get block hash for the given height using typed response
            BlockHashResponse hash_resp = client.get_block_hash(height);
            std::string block_hash = hash_resp.hash;

            // Get full block data using typed response
            YcashBlock block_data = client.get_block_by_hash(block_hash);

            glz::generic::object_t response;
            response["status"] = "success";
            response["data"]["height"] = height;
            response["data"]["hash"] = block_data.hash;
            response["data"]["time"] = block_data.time;
            response["data"]["tx_count"] = static_cast<int>(block_data.tx.size());
            response["data"]["size"] = block_data.size;
            response["data"]["difficulty"] = block_data.difficulty;
            response["data"]["previous_hash"] = block_data.previousblockhash;
            response["data"]["next_hash"] = block_data.nextblockhash.value_or("");
            response["data"]["merkle_root"] = block_data.merkleroot;
            response["data"]["nonce"] = block_data.nonce;
            response["data"]["version"] = block_data.version;

            auto ec = glz::write_json(response, resp_body);
            auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
            resp->setBody(resp_body);
            callback(resp);
        }
        catch (const std::exception& e)
        {
            logger.errorf("Failed to get block at height {}: {}", height, e.what());
            glz::generic::object_t response {{"status", "error"}, {"message", "Block not found"}};
            auto ec = glz::write_json(response, resp_body);
            auto resp = HttpResponse::newHttpResponse(drogon::k404NotFound, drogon::CT_APPLICATION_JSON);
            resp->setBody(resp_body);
            callback(resp);
            return;
        }

    }
    catch (const std::exception& e)
    {
        Logger& logger = Logger::instance();
        logger.errorf("Exception in get_block_by_height: {}", e.what());
        glz::generic::object_t response {{"status", "error"}, {"message", "Server error"}};
        auto ec = glz::write_json(response, resp_body);        
        auto resp = HttpResponse::newHttpResponse(k500InternalServerError, CT_APPLICATION_JSON);
        resp->setBody(resp_body);
        callback(resp);
    }
}


void BlockController::get_block_by_hash(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
{
    Logger& logger {Logger::instance()};
    HttpResponsePtr resp {HttpResponse::newHttpResponse()};
    glz::generic::object_t response {};
    std::string resp_body {};

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();

        try
        {
            // Get block by hash using typed response
            YcashBlock block_data = client.get_block_by_hash(hash);

            response["status"] = "success";
            response["data"]["hash"] = block_data.hash;
            response["data"]["height"] = block_data.height;
            response["data"]["time"] = block_data.time;
            response["data"]["tx_count"] = static_cast<int>(block_data.tx.size());
            response["data"]["size"] = block_data.size;
            response["data"]["difficulty"] = block_data.difficulty;
            response["data"]["previous_hash"] = block_data.previousblockhash;
            response["data"]["next_hash"] = block_data.nextblockhash.value_or("");
            response["data"]["merkle_root"] = block_data.merkleroot;
            response["data"]["nonce"] = block_data.nonce;
            response["data"]["version"] = block_data.version;
            
            auto ec = glz::write_json(response, resp_body);
            resp->setBody(resp_body);
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            callback(resp);
        }
        catch (const std::exception& e)
        {
            logger.warnf("Failed to get block by hash {}: {}", hash, e.what());
            response["status"] = "error";
            response["message"] = "Block not found";
            
            auto ec = glz::write_json(response, resp_body);
            resp->setBody(resp_body);
            resp->setStatusCode(k404NotFound);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            callback(resp);
            return;
        }

    }
    catch (const std::exception& e)
    {
        Logger& logger = Logger::instance();
        logger.errorf("Exception in get_block_by_hash: {}", e.what());
        response["status"] = "error";
        response["message"] = "Server error";

        auto ec = glz::write_json(response, resp_body);
        resp->setBody(resp_body);
        resp->setStatusCode(k500InternalServerError);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        callback(resp);
    }
}


void BlockController::get_block_verbose(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
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

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();
        YcashBlockVerbose block = client.get_block_verbose_by_hash(hash);

        // Fetch DB-enriched fields (miner address, reward)
        std::string miner_address;
        double claimed_reward = 0.0;
        try
        {
            QueryConnectionGuard db;
            // Use bytea literal so idx_blocks_hash index is used instead of full table scan
            std::string q = std::format(
                "SELECT miner_address, claimed_reward_block FROM blocks "
                "WHERE hash = '\\x{}' LIMIT 1", hash);
            PGresult* res = PQexec(db.conn(), q.c_str());
            if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
            {
                miner_address = PQgetvalue(res, 0, 0);
                claimed_reward = std::stod(PQgetvalue(res, 0, 1)) / 100000000.0;
            }
            PQclear(res);
        }
        catch (const std::exception& e)
        {
            logger.warnf("DB lookup failed for block {}: {}", hash, e.what());
        }

        // Block header fields
        response["status"] = "success";
        auto& d = response["data"];
        d["hash"]              = block.hash;
        d["height"]            = block.height;
        d["confirmations"]     = block.confirmations;
        d["time"]              = block.time;
        d["size"]              = block.size;
        d["difficulty"]        = block.difficulty;
        d["bits"]              = block.bits;
        d["merkle_root"]       = block.merkleroot;
        d["nonce"]             = block.nonce;
        d["version"]           = block.version;
        d["previous_hash"]     = block.previousblockhash;
        d["next_hash"]         = block.nextblockhash.value_or("");
        d["block_fee"]         = block.blockfee;
        d["chain_supply"]      = block.chainSupply.chainValue;
        d["miner_address"]     = miner_address;
        d["claimed_reward"]    = claimed_reward;

        // Value pools
        d["value_pools"] = glz::generic::array_t {};
        for (const auto& pool : block.valuePools)
        {
            glz::generic::object_t p;
            p["id"]         = pool.id;
            p["monitored"]  = pool.monitored;
            p["chain_value"] = pool.chainValue;
            d["value_pools"].get_array().push_back(p);
        }

        // Transactions
        d["tx"] = glz::generic::array_t {};
        for (const auto& tx : block.tx)
        {
            glz::generic::object_t t;
            t["txid"]            = tx.txid;
            t["size"]            = tx.size;
            t["value_balance"]   = tx.valueBalance;
            t["expiry_height"]   = tx.expiryheight;

            // Inputs
            t["vin"] = glz::generic::array_t {};
            for (const auto& in : tx.vin)
            {
                glz::generic::object_t v;
                v["coinbase"] = in.coinbase;
                v["txid"]     = in.txid;
                v["vout"]     = static_cast<int>(in.vout);
                v["address"]  = in.address;
                v["value"]    = in.value;
                t["vin"].get_array().push_back(v);
            }

            // Outputs
            t["vout"] = glz::generic::array_t {};
            for (const auto& out : tx.vout)
            {
                glz::generic::object_t v;
                v["n"]         = out.n;
                v["value"]     = out.value;
                v["addresses"] = glz::generic::array_t {};
                for (const auto& addr : out.scriptPubKey.addresses)
                    v["addresses"].get_array().push_back(addr);
                v["type"]      = out.scriptPubKey.spk_type;
                v["spent_txid"] = out.spentTxId;
                t["vout"].get_array().push_back(v);
            }

            // Shielded counts
            t["sapling_spends"]  = static_cast<int>(tx.vShieldedSpend.size());
            t["sapling_outputs"] = static_cast<int>(tx.vShieldedOutput.size());

            // Sprout joinsplits
            t["joinsplits"] = glz::generic::array_t {};
            for (const auto& js : tx.vjoinsplit)
            {
                glz::generic::object_t j;
                j["vpub_old"] = js.vpub_old;
                j["vpub_new"] = js.vpub_new;
                t["joinsplits"].get_array().push_back(j);
            }

            d["tx"].get_array().push_back(t);
        }

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_block_verbose: {}", e.what());
        response["status"] = "error";
        response["message"] = "Block not found or server error";
        send(drogon::k404NotFound);
    }
}


void BlockController::get_block_raw(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const
{
    Logger& logger {Logger::instance()};

    try
    {
        YcashRpcClient& client = YcashRpcClient::thread_instance();
        std::string raw = client.get_block_raw_json(hash);
        auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
        resp->setBody(raw);
        callback(resp);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_block_raw: {}", e.what());
        std::string body = R"({"status":"error","message":"Block not found"})";
        auto resp = HttpResponse::newHttpResponse(drogon::k404NotFound, drogon::CT_APPLICATION_JSON);
        resp->setBody(body);
        callback(resp);
    }
}
