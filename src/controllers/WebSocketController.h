#pragma once

#include <drogon/WebSocketController.h>

using namespace drogon;

class InzyghtWebSocketController : public WebSocketController<InzyghtWebSocketController>
{
    public:
        InzyghtWebSocketController() = default;

        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws");
        WS_PATH_LIST_END

        virtual void handleNewMessage(const WebSocketConnectionPtr& ws_conn_ptr, std::string&& message, const WebSocketMessageType& type) override;

        virtual void handleConnectionClosed(const WebSocketConnectionPtr& ws_conn_ptr) override;

        virtual void handleNewConnection(const HttpRequestPtr& req, const WebSocketConnectionPtr& ws_conn_ptr) override;
};
