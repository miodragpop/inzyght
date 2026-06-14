#include "PageController.h"

void PageController::serve_block_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& /*id*/) const
{
    auto resp = HttpResponse::newFileResponse("./public/block.html");
    callback(resp);
}

void PageController::serve_transaction_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& /*id*/) const
{
    auto resp = HttpResponse::newFileResponse("./public/transaction.html");
    callback(resp);
}

void PageController::serve_address_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& /*id*/) const
{
    auto resp = HttpResponse::newFileResponse("./public/address.html");
    callback(resp);
}

void PageController::serve_blocks_list_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/blocks.html");
    callback(resp);
}

void PageController::serve_transactions_list_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/transactions.html");
    callback(resp);
}

void PageController::serve_api_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/api.html");
    callback(resp);
}

void PageController::serve_mempool_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/mempool.html");
    callback(resp);
}

void PageController::serve_supply_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/supply.html");
    callback(resp);
}

void PageController::serve_difficulty_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/difficulty.html");
    callback(resp);
}

void PageController::serve_blocksize_page(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    auto resp = HttpResponse::newFileResponse("./public/blocksize.html");
    callback(resp);
}
