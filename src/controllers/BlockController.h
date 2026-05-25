#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class BlockController : public HttpController<BlockController>
{
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(BlockController::get_latest_blocks,   "/api/v1/blocks",                Get, "ApiRateLimiter");
        ADD_METHOD_TO(BlockController::get_block_by_height, "/api/v1/blocks/{height}",        Get, "ApiRateLimiter");
        ADD_METHOD_TO(BlockController::get_block_by_hash,   "/api/v1/blocks/hash/{hash}",     Get, "ApiRateLimiter");
        ADD_METHOD_TO(BlockController::get_block_verbose,   "/api/v1/blocks/verbose/{hash}",  Get, "ApiRateLimiter");
        ADD_METHOD_TO(BlockController::get_block_raw,       "/api/v1/blocks/raw/{hash}",      Get, "ApiRateLimiter");
        METHOD_LIST_END

        void get_latest_blocks(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_block_by_height(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, int height) const;
        
        void get_block_by_hash(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;

        void get_block_verbose(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;

        void get_block_raw(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;
};
