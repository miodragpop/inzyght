#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class TransactionController : public HttpController<TransactionController>
{
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(TransactionController::get_latest_transactions, "/api/v1/transactions",                Get, "ApiRateLimiter");
        ADD_METHOD_TO(TransactionController::get_all_transactions,    "/api/v1/transactions/all",            Get, "ApiRateLimiter");
        ADD_METHOD_TO(TransactionController::get_mempool,             "/api/v1/transactions/mempool",        Get, "ApiRateLimiter");
        ADD_METHOD_TO(TransactionController::get_transaction_by_hash, "/api/v1/transactions/{hash}",         Get, "ApiRateLimiter");
        ADD_METHOD_TO(TransactionController::get_transaction_verbose, "/api/v1/transactions/verbose/{hash}", Get, "ApiRateLimiter");
        ADD_METHOD_TO(TransactionController::get_transaction_raw,     "/api/v1/transactions/raw/{hash}",     Get, "ApiRateLimiter");
        METHOD_LIST_END

        void get_latest_transactions(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
        void get_all_transactions(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
        void get_mempool(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;

        void get_transaction_by_hash(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;
        void get_transaction_verbose(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;
        void get_transaction_raw(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& hash) const;
};
