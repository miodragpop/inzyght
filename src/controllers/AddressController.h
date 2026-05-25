#pragma once

#include <drogon/HttpController.h>
using namespace drogon;

class AddressController : public HttpController<AddressController>
{
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(AddressController::get_address_info,         "/api/v1/addresses/{address}",              Get, "AddressRateLimiter");
        ADD_METHOD_TO(AddressController::get_address_transactions,  "/api/v1/addresses/{address}/transactions", Get, "AddressRateLimiter");
        METHOD_LIST_END

        void get_address_info(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& address) const;

        void get_address_transactions(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& address) const;
};
