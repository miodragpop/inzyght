#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class NetworkController : public HttpController<NetworkController>
{
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(NetworkController::get_network_info,    "/api/v1/network/info",       Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_blockchain_info, "/api/v1/network/blockchain", Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_peers,           "/api/v1/network/peers",      Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_version,         "/api/v1/version",            Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_supply_series,   "/api/v1/supply/series",      Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_difficulty_series,"/api/v1/difficulty/series",  Get, "ApiRateLimiter");
        ADD_METHOD_TO(NetworkController::get_blocksize_series, "/api/v1/blocksize/series",   Get, "ApiRateLimiter");
        METHOD_LIST_END

        void get_network_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_blockchain_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_peers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_version(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_supply_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_difficulty_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_blocksize_series(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

};
