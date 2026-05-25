#include "SyncController.h"
#include "services/BlockIndexer.h"
#include "Logger.h"
#include <drogon/HttpTypes.h>
#include "glaze/json/generic.hpp"
#include <format>

void SyncController::get_progress(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) const
{
    glz::generic response;
    std::string response_str;
    
    try
    {
        response["status"] = "success";

        // Get sync progress from BlockIndexer
        BlockIndexer& indexer = BlockIndexer::instance();

        glz::generic block_progress;
        block_progress["status"] = indexer.is_running() ? "syncing" : "idle";
        block_progress["percentage"] = indexer.get_sync_progress();
        block_progress["current_height"] = indexer.get_current_height();
        block_progress["avg_speed"] = indexer.get_average_sync_speed();  // Total blocks / total time from sync start
        block_progress["current_speed"] = indexer.get_current_batch_speed();  // Blocks in last batch / batch processing time
        block_progress["eta_seconds"] = (Json::Int64)indexer.get_estimated_time_remaining();  // ETA based on recent speed
        block_progress["sync_start_time"] = indexer.get_sync_start_time();

        response["data"]["blocks"] = block_progress;

        auto ec = glz::write_json(response, response_str);
        auto resp = HttpResponse::newHttpResponse(drogon::k200OK, drogon::CT_APPLICATION_JSON);
        resp->setBody(response_str);
        callback(resp);

    }
    catch (const std::exception& e)
    {
        Logger::instance().errorf("SyncController: Exception in get_progress: {}", e.what());

        response["status"] = "error";
        response["message"] = "Failed to get sync progress";

        auto ec = glz::write_json(response, response_str);
        auto resp = HttpResponse::newHttpResponse(drogon::k500InternalServerError, drogon::CT_APPLICATION_JSON);
        resp->setBody(response_str);
        callback(resp);
    }
}
