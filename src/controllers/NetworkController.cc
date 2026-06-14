#include "NetworkController.h"
#include "models/RpcResponses.h"
#include "services/BlockchainState.h"
#include "services/GeoLocationService.h"
#include "glaze/json/generic.hpp"
#include "Logger.h"
#include "Version.h"
#include "services/BlockIndexer.h"
#include "orm/PostgresPool.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <algorithm>
#include <format>
#include <vector>


void NetworkController::get_network_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger {Logger::instance()};
    BlockchainState& current_state {BlockchainState::instance()};

    // Get network info from in-memory state (updated by ZeroMQ events)
    glz::generic info = current_state.get_proxy_network_info();

    // If state is empty, it hasn't been populated yet - fetch from state cache
    if (info.empty() || !info.contains("blocks"))
    {
        logger.debug("Network state not yet populated, fetching from state cache");

        InfoResponseExtended cached_info_extended {current_state.get_cached_info_extended()};

        info["version"] = cached_info_extended.version;
        info["subversion"] = cached_info_extended.subversion;
        info["protocolversion"] = cached_info_extended.protocolversion;
        info["blocks"] = cached_info_extended.blocks;
        info["connections"] = cached_info_extended.connections;
        info["total_transactions"] = cached_info_extended.total_transactions;
        info["difficulty"] = cached_info_extended.difficulty;
        info["testnet"] = cached_info_extended.testnet;
        info["chain_supply"] = cached_info_extended.chain_supply;
        info["transparent_supply"] = cached_info_extended.transparent_supply;
        info["sprout_supply"] = cached_info_extended.sprout_supply;
        info["sapling_supply"] = cached_info_extended.sapling_supply;
        info["networksolps"] = cached_info_extended.networksolps;
        info["mempool_count"] = static_cast<int64_t>(current_state.get_mempool_count());

        // Cache it in state for next requests
        current_state.set_proxy_network_info(info);
    }

    response["status"] = "success";
    response["data"]["version"] = info["version"];
    response["data"]["subversion"] = info["subversion"];
    response["data"]["explorer_version"] = inzyght::k_version;
    response["data"]["protocol_version"] = info["protocolversion"];
    response["data"]["blocks"] = info["blocks"];
    response["data"]["connections"] = info["connections"];
    response["data"]["total_transactions"] = info["total_transactions"];
    response["data"]["difficulty"] = info["difficulty"];
    response["data"]["testnet"] = info["testnet"];
    response["data"]["chain_supply"] = info["chain_supply"];
    response["data"]["transparent_supply"] = info["transparent_supply"];
    response["data"]["sprout_supply"] = info["sprout_supply"];
    response["data"]["sapling_supply"] = info["sapling_supply"];
    response["data"]["networksolps"] = info["networksolps"];
    response["data"]["mempool_count"] = static_cast<int64_t>(current_state.get_mempool_count());
    // Snapshot freshness — lets the frontend flag data as frozen when ycashd
    // stops answering RPC (the cache keeps the last good snapshot).
    response["data"]["as_of_age_seconds"] = glz::generic_i64(current_state.get_snapshot_age_seconds());
    response["data"]["stale"] = current_state.is_snapshot_stale();

    auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
    auto ec = glz::write_json(response, resp_body);
    resp->setBody(resp_body);
    callback(resp);
}


void NetworkController::get_blockchain_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger = Logger::instance();

    try
    {
        // Served from BlockchainState's cache, which is refreshed by
        // EventProcessor on every ZeroMQ block event (at most ~75s stale on
        // ycash mainnet). No RPC call is issued here, so a frontend polling
        // this endpoint cannot add load to ycashd during sync.
        BlockchainInfoResponse chain_info {BlockchainState::instance().get_cached_blockchain_info()};

        response["status"] = "success";
        response["data"]["chain"] = chain_info.chain;
        response["data"]["blocks"] = chain_info.blocks;
        response["data"]["headers"] = chain_info.headers;
        response["data"]["bestblockhash"] = chain_info.bestblockhash;
        response["data"]["difficulty"] = chain_info.difficulty;
        response["data"]["verificationprogress"] = chain_info.verificationprogress;
        response["data"]["initial_block_download_complete"] = chain_info.initial_block_download_complete;
        response["data"]["chainwork"] = chain_info.chainwork;
        response["data"]["estimatedheight"] = chain_info.estimatedheight;
        response["data"]["commitments"] = chain_info.commitments;
        response["data"]["transactions"] = chain_info.transactions;
        response["data"]["chain_supply"] = chain_info.chainSupply.chainValue;

        auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
        resp_body = glz::write_json(response).value_or("error");
        resp->setBody(resp_body);
        callback(resp);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_blockchain_info: {}", e.what());
        response["status"] = "error";
        response["message"] = "Server error";
        auto resp = HttpResponse::newHttpResponse(drogon::k500InternalServerError, drogon::CT_APPLICATION_JSON);
        auto ec = glz::write_json(response, resp_body);
        resp->setBody(resp_body);
        callback(resp);
    }
}


void NetworkController::get_peers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger = Logger::instance();

    try
    {
        std::vector<PeerInfoResponse> peers {BlockchainState::instance().get_cached_peers()};

        // Extract IPs for geolocation
        std::vector<std::string> ips;
        ips.reserve(peers.size());
        for (const auto& p : peers)
        {
            ips.push_back(GeoLocationService::extract_ip(p.addr));            
        }

        std::vector<GeoData> geo {GeoLocationService::instance().geolocate(ips)};

        glz::generic::array_t peer_list;
        for (size_t i = 0; i < peers.size(); ++i)
        {
            const auto& p = peers[i];
            const auto& g = geo[i];
            glz::generic entry;
            entry["addr"]          = p.addr;
            entry["ip"]            = ips[i];
            entry["inbound"]       = p.inbound;
            entry["ping_ms"]       = p.pingtime * 1000.0;
            entry["version"]       = p.version;
            entry["subver"]        = p.subver;
            entry["synced_blocks"] = p.synced_blocks;
            entry["country"]       = g.country;
            entry["country_code"]  = g.countryCode;
            entry["city"]          = g.city;
            entry["lat"]           = g.lat;
            entry["lon"]           = g.lon;
            peer_list.push_back(entry);
        }

        response["status"] = "success";
        response["data"]   = peer_list;
        // Snapshot freshness, same semantics as /network/info: the peer list
        // is frozen at its last successful refresh when ycashd is unreachable.
        response["as_of_age_seconds"] = glz::generic_i64(BlockchainState::instance().get_snapshot_age_seconds());
        response["stale"] = BlockchainState::instance().is_snapshot_stale();

        auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
        resp_body = glz::write_json(response).value_or("error");
        resp->setBody(resp_body);
        callback(resp);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_peers: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        auto resp = HttpResponse::newHttpResponse(drogon::k500InternalServerError, drogon::CT_APPLICATION_JSON);
        resp_body = glz::write_json(response).value_or("error");
        resp->setBody(resp_body);
        callback(resp);
    }
}


void NetworkController::get_version(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    response["status"] = "success";
    response["data"]["version"] = inzyght::k_version;

    std::string resp_body = glz::write_json(response).value_or("{}");
    auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
    resp->setBody(resp_body);
    callback(resp);
}


// ── Supply-by-pool time series ─────────────────────────────────────────────────
//
// Returns cumulative supply (transparent / sprout / sapling / total) over a
// height range, in YEC. The chart drives this: it passes the visible height
// window (from/to) plus a target point count (~ the pixel width). When the
// window spans more blocks than the display can show we sample one block per
// stride; zoom in and the window shrinks until stride==1, i.e. full per-block
// resolution.
void NetworkController::get_supply_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger = Logger::instance();

    auto send = [&](drogon::HttpStatusCode code) {
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        glz::write_json(response, resp_body);
        resp->setBody(resp_body);
        callback(resp);
    };

    auto empty_series = [&]() {
        response["t"]           = glz::generic::array_t{};
        response["h"]           = glz::generic::array_t{};
        response["transparent"] = glz::generic::array_t{};
        response["sprout"]      = glz::generic::array_t{};
        response["sapling"]     = glz::generic::array_t{};
        response["total"]       = glz::generic::array_t{};
    };

    try
    {
        if (BlockIndexer::instance().is_syncing())
        {
            response["status"]  = "success";
            response["syncing"] = true;
            empty_series();
            send(drogon::k200OK);
            return;
        }

        const auto& params = req->getParameters();

        QueryConnectionGuard db;

        // Resolve the chain's height bounds once so we can clamp the request.
        long chain_lo = 0, chain_hi = 0;
        bool have_bounds = false;
        {
            PGresult* b = PQexec(db.conn(), "SELECT MIN(height), MAX(height) FROM blocks");
            if (PQresultStatus(b) == PGRES_TUPLES_OK && PQntuples(b) == 1 && !PQgetisnull(b, 0, 0))
            {
                chain_lo = std::stol(PQgetvalue(b, 0, 0));
                chain_hi = std::stol(PQgetvalue(b, 0, 1));
                have_bounds = true;
            }
            PQclear(b);
        }

        if (!have_bounds)
        {
            // Empty table — nothing indexed yet.
            response["status"] = "success";
            empty_series();
            send(drogon::k200OK);
            return;
        }

        auto parse_long = [&](const char* key, long fallback) -> long {
            auto it = params.find(key);
            if (it == params.end()) return fallback;
            try { return std::stol(it->second); } catch (...) { return fallback; }
        };

        long from = std::clamp(parse_long("from", chain_lo), chain_lo, chain_hi);
        long to   = std::clamp(parse_long("to",   chain_hi), chain_lo, chain_hi);
        if (from > to) std::swap(from, to);

        long points = std::clamp(parse_long("points", 2000L), 100L, 4000L);

        const long span   = to - from + 1;           // inclusive block count
        const long bucket = std::max<long>(1, (span + points - 1) / points);

        // Even-stride sampling: take the last height of each `bucket`-sized
        // stride, i.e. (height-from) % bucket == bucket-1, plus the final block
        // so the most recent value always shows. Because `height` is a dense PK
        // this is an index-friendly range scan that touches only the sampled
        // rows — far cheaper than a DISTINCT ON, which would sort the whole
        // range. For cumulative supply, evenly spaced samples are a faithful
        // summary. When bucket==1 (fully zoomed in) every block is returned.
        std::string query = std::format(
            "SELECT height, timestamp, transparent_supply, sprout_supply, sapling_supply, chain_supply "
            "FROM blocks "
            "WHERE height BETWEEN {0} AND {1} "
            "  AND ((height - {0}) % {2} = {2} - 1 OR height = {1}) "
            "ORDER BY height ASC",
            from, to, bucket);

        PGresult* res = PQexec(db.conn(), query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            logger.errorf("Supply series query failed: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"]  = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        const int n = PQntuples(res);
        glz::generic::array_t t, h, transparent, sprout, sapling, total;
        t.reserve(n); h.reserve(n); transparent.reserve(n); sprout.reserve(n); sapling.reserve(n); total.reserve(n);

        constexpr double COIN = 100000000.0;  // zat → YEC
        for (int i = 0; i < n; ++i)
        {
            // array_t is a homogeneous f64 array. Heights (~1e6) and unix
            // timestamps (~1.7e9) are well within exact f64 integer range.
            h.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 0))));
            t.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 1))));
            transparent.push_back(std::stod(PQgetvalue(res, i, 2)) / COIN);
            sprout.push_back(std::stod(PQgetvalue(res, i, 3)) / COIN);
            sapling.push_back(std::stod(PQgetvalue(res, i, 4)) / COIN);
            total.push_back(std::stod(PQgetvalue(res, i, 5)) / COIN);
        }
        PQclear(res);

        response["status"]      = "success";
        response["from"]        = glz::generic_i64(from);
        response["to"]          = glz::generic_i64(to);
        response["chain_min"]   = glz::generic_i64(chain_lo);
        response["chain_max"]   = glz::generic_i64(chain_hi);
        response["bucket"]      = glz::generic_i64(bucket);
        response["t"]           = std::move(t);
        response["h"]           = std::move(h);
        response["transparent"] = std::move(transparent);
        response["sprout"]      = std::move(sprout);
        response["sapling"]     = std::move(sapling);
        response["total"]       = std::move(total);

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_supply_series: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}


// ── Mining difficulty time series ──────────────────────────────────────────────
//
// Same shape and adaptive downsampling as get_supply_series, but difficulty is
// NOT cumulative — it varies block to block — so a "last block per stride"
// sample would alias spikes. Instead we average difficulty over each stride,
// which is a faithful representative of the bucket and still an index-friendly
// grouped scan on the height PK. When bucket==1 (fully zoomed in) the average is
// over a single block, i.e. the exact per-block value.
void NetworkController::get_difficulty_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger = Logger::instance();

    auto send = [&](drogon::HttpStatusCode code) {
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        glz::write_json(response, resp_body);
        resp->setBody(resp_body);
        callback(resp);
    };

    auto empty_series = [&]() {
        response["t"]          = glz::generic::array_t{};
        response["h"]          = glz::generic::array_t{};
        response["difficulty"] = glz::generic::array_t{};
    };

    try
    {
        if (BlockIndexer::instance().is_syncing())
        {
            response["status"]  = "success";
            response["syncing"] = true;
            empty_series();
            send(drogon::k200OK);
            return;
        }

        const auto& params = req->getParameters();

        QueryConnectionGuard db;

        long chain_lo = 0, chain_hi = 0;
        bool have_bounds = false;
        {
            PGresult* b = PQexec(db.conn(), "SELECT MIN(height), MAX(height) FROM blocks");
            if (PQresultStatus(b) == PGRES_TUPLES_OK && PQntuples(b) == 1 && !PQgetisnull(b, 0, 0))
            {
                chain_lo = std::stol(PQgetvalue(b, 0, 0));
                chain_hi = std::stol(PQgetvalue(b, 0, 1));
                have_bounds = true;
            }
            PQclear(b);
        }

        if (!have_bounds)
        {
            response["status"] = "success";
            empty_series();
            send(drogon::k200OK);
            return;
        }

        auto parse_long = [&](const char* key, long fallback) -> long {
            auto it = params.find(key);
            if (it == params.end()) return fallback;
            try { return std::stol(it->second); } catch (...) { return fallback; }
        };

        long from = std::clamp(parse_long("from", chain_lo), chain_lo, chain_hi);
        long to   = std::clamp(parse_long("to",   chain_hi), chain_lo, chain_hi);
        if (from > to) std::swap(from, to);

        long points = std::clamp(parse_long("points", 2000L), 100L, 4000L);

        const long span   = to - from + 1;
        const long bucket = std::max<long>(1, (span + points - 1) / points);

        // Group blocks into `bucket`-sized strides and average difficulty per
        // stride. Representative height/time = the stride's last block (MAX), so
        // the x positions match what the supply chart produces for the same
        // window. Ordered chronologically for the chart.
        std::string query = std::format(
            "SELECT MAX(height) AS h, MAX(timestamp) AS t, AVG(difficulty) AS d "
            "FROM blocks "
            "WHERE height BETWEEN {0} AND {1} "
            "GROUP BY (height - {0}) / {2} "
            "ORDER BY h ASC",
            from, to, bucket);

        PGresult* res = PQexec(db.conn(), query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            logger.errorf("Difficulty series query failed: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"]  = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        const int n = PQntuples(res);
        glz::generic::array_t t, h, difficulty;
        t.reserve(n); h.reserve(n); difficulty.reserve(n);

        for (int i = 0; i < n; ++i)
        {
            h.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 0))));
            t.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 1))));
            difficulty.push_back(std::stod(PQgetvalue(res, i, 2)));
        }
        PQclear(res);

        response["status"]     = "success";
        response["from"]       = glz::generic_i64(from);
        response["to"]         = glz::generic_i64(to);
        response["chain_min"]  = glz::generic_i64(chain_lo);
        response["chain_max"]  = glz::generic_i64(chain_hi);
        response["bucket"]     = glz::generic_i64(bucket);
        response["t"]          = std::move(t);
        response["h"]          = std::move(h);
        response["difficulty"] = std::move(difficulty);

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_difficulty_series: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}


// ── Block size time series ─────────────────────────────────────────────────────
//
// Same shape and adaptive downsampling as get_difficulty_series. Block size is
// not cumulative, so we average it over each stride (a faithful representative
// that won't alias spikes), with the stride's last block giving the height/time
// position. Sizes are returned in bytes; the frontend formats to KB/MB.
void NetworkController::get_blocksize_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic::object_t response {};
    std::string resp_body {};
    Logger& logger = Logger::instance();

    auto send = [&](drogon::HttpStatusCode code) {
        auto resp = HttpResponse::newHttpResponse(code, drogon::CT_APPLICATION_JSON);
        glz::write_json(response, resp_body);
        resp->setBody(resp_body);
        callback(resp);
    };

    auto empty_series = [&]() {
        response["t"]    = glz::generic::array_t{};
        response["h"]    = glz::generic::array_t{};
        response["size"] = glz::generic::array_t{};
    };

    try
    {
        if (BlockIndexer::instance().is_syncing())
        {
            response["status"]  = "success";
            response["syncing"] = true;
            empty_series();
            send(drogon::k200OK);
            return;
        }

        const auto& params = req->getParameters();

        QueryConnectionGuard db;

        long chain_lo = 0, chain_hi = 0;
        bool have_bounds = false;
        {
            PGresult* b = PQexec(db.conn(), "SELECT MIN(height), MAX(height) FROM blocks");
            if (PQresultStatus(b) == PGRES_TUPLES_OK && PQntuples(b) == 1 && !PQgetisnull(b, 0, 0))
            {
                chain_lo = std::stol(PQgetvalue(b, 0, 0));
                chain_hi = std::stol(PQgetvalue(b, 0, 1));
                have_bounds = true;
            }
            PQclear(b);
        }

        if (!have_bounds)
        {
            response["status"] = "success";
            empty_series();
            send(drogon::k200OK);
            return;
        }

        auto parse_long = [&](const char* key, long fallback) -> long {
            auto it = params.find(key);
            if (it == params.end()) return fallback;
            try { return std::stol(it->second); } catch (...) { return fallback; }
        };

        long from = std::clamp(parse_long("from", chain_lo), chain_lo, chain_hi);
        long to   = std::clamp(parse_long("to",   chain_hi), chain_lo, chain_hi);
        if (from > to) std::swap(from, to);

        long points = std::clamp(parse_long("points", 2000L), 100L, 4000L);

        const long span   = to - from + 1;
        const long bucket = std::max<long>(1, (span + points - 1) / points);

        std::string query = std::format(
            "SELECT MAX(height) AS h, MAX(timestamp) AS t, AVG(size) AS s "
            "FROM blocks "
            "WHERE height BETWEEN {0} AND {1} "
            "GROUP BY (height - {0}) / {2} "
            "ORDER BY h ASC",
            from, to, bucket);

        PGresult* res = PQexec(db.conn(), query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            logger.errorf("Block size series query failed: {}", PQerrorMessage(db.conn()));
            PQclear(res);
            response["status"]  = "error";
            response["message"] = "Database query failed";
            send(drogon::k500InternalServerError);
            return;
        }

        const int n = PQntuples(res);
        glz::generic::array_t t, h, size;
        t.reserve(n); h.reserve(n); size.reserve(n);

        for (int i = 0; i < n; ++i)
        {
            h.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 0))));
            t.push_back(static_cast<double>(std::stoll(PQgetvalue(res, i, 1))));
            size.push_back(std::stod(PQgetvalue(res, i, 2)));
        }
        PQclear(res);

        response["status"]    = "success";
        response["from"]      = glz::generic_i64(from);
        response["to"]        = glz::generic_i64(to);
        response["chain_min"] = glz::generic_i64(chain_lo);
        response["chain_max"] = glz::generic_i64(chain_hi);
        response["bucket"]    = glz::generic_i64(bucket);
        response["t"]         = std::move(t);
        response["h"]         = std::move(h);
        response["size"]      = std::move(size);

        send(drogon::k200OK);
    }
    catch (const std::exception& e)
    {
        logger.errorf("Exception in get_blocksize_series: {}", e.what());
        response["status"]  = "error";
        response["message"] = "Server error";
        send(drogon::k500InternalServerError);
    }
}


