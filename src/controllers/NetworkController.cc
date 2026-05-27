#include "NetworkController.h"
#include "models/RpcResponses.h"
#include "services/BlockchainState.h"
#include "services/GeoLocationService.h"
#include "glaze/json/generic.hpp"
#include "Logger.h"
#include "Version.h"
#include <drogon/HttpTypes.h>


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


