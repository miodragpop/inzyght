#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class PageController : public HttpController<PageController>
{
public:
    METHOD_LIST_BEGIN
    // Page routes are crawler targets too, so they carry the same per-IP limits
    // as the API. /address/* drives the expensive address path → strict tier.
    ADD_METHOD_TO(PageController::serve_block_page,        "/block/{id}",       Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_transaction_page,  "/transaction/{id}", Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_address_page,      "/address/{id}",     Get, "AddressRateLimiter");
    ADD_METHOD_TO(PageController::serve_blocks_list_page,        "/blocks",       Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_transactions_list_page,  "/transactions", Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_api_page,                "/api",          Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_mempool_page,            "/mempool",      Get, "ApiRateLimiter");
    ADD_METHOD_TO(PageController::serve_supply_page,             "/supply",       Get, "ApiRateLimiter");
    METHOD_LIST_END

    void serve_block_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) const;
    void serve_transaction_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) const;
    void serve_address_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) const;
    void serve_blocks_list_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
    void serve_transactions_list_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
    void serve_api_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
    void serve_mempool_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
    void serve_supply_page(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
};
