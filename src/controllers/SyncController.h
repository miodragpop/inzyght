#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class SyncController : public drogon::HttpController<SyncController>
{
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(SyncController::get_progress, "/api/v1/sync/progress", Get);
        METHOD_LIST_END

        // Get sync progress for indexing
        void get_progress(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const;
};
