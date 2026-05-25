#include "WebSocketController.h"
#include "services/EventBroadcaster.h"
#include "Logger.h"
#include "glaze/json/generic.hpp"

void InzyghtWebSocketController::handleNewConnection(const HttpRequestPtr& req, const WebSocketConnectionPtr& ws_conn_ptr)
{
    Logger::instance().infof("WebSocket client connected: {} from {}", req->getHeader("User-Agent"), ws_conn_ptr->peerAddr().toIpPort());
    EventBroadcaster::instance().register_client(ws_conn_ptr);

    // Send welcome message
    glz::generic welcome;
    //std::string json_str;
    welcome["type"] = "connected";
    welcome["message"] = "Connected to Inzyght WebSocket";
    welcome["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

    //auto ec = glz::write_json(welcome, json_str);
    ws_conn_ptr->send(glz::write_json(welcome).value_or(""));
}


void InzyghtWebSocketController::handleNewMessage(const WebSocketConnectionPtr& ws_conn_ptr, std::string&& message, const WebSocketMessageType& type)
{
    Logger& logger = Logger::instance();

    // Only process text frames (type == WebSocketMessageType::Text)
    // Ignore binary frames, control frames, and other non-text message types
    if (type != WebSocketMessageType::Text)
    {
        logger.debugf("WebSocket: Ignoring non-text frame from {} (type: {})", ws_conn_ptr->peerAddr().toIpPort(), static_cast<int>(type));
        return;
    }

    try
    {
        // Skip empty or whitespace-only messages
        if (message.empty() || message.find_first_not_of(" \t\n\r\f\v") == std::string::npos)
        {
            logger.debug("Skipping empty WebSocket message");
            return;
        }

        logger.debugf("WebSocket message received from {}: {}", ws_conn_ptr->peerAddr().toIpPort(), (message.length() > 200 ? message.substr(0, 200) + "..." : message));

        glz::generic incoming_message;
        auto ec = glz::read_json(incoming_message, message);

        if (ec)
        {
            logger.warnf("Invalid JSON message from WebSocket client: {}", glz::format_error(ec));
            glz::generic error_response;
            //std::string error_json_str;

            error_response["type"] = "error";
            error_response["message"] = "Invalid JSON format";

            //auto ec_1 = glz::write_json(error_response, error_json_str);
            ws_conn_ptr->send(glz::write_json(error_response).value_or(""));
            return;
        }

        // Handle message based on type
        std::string msg_type = incoming_message["type"].get_string();

        if (msg_type == "subscribe")
        {
            std::string channel = incoming_message["channel"].get_string();
            glz::generic response;
            //std::string response_json;

            response["type"] = "subscription_ack";
            response["channel"] = channel;
            response["status"] = "subscribed";

            //ec = glz::write_json(response, response_json);
            ws_conn_ptr->send(glz::write_json(response).value_or(""));
            logger.debugf("Client subscribed to: {}", channel);
        }
        else if (msg_type == "unsubscribe")
        {
            std::string channel = incoming_message["channel"].get_string();
            glz::generic response;
            //std::string response_json;

            response["type"] = "subscription_ack";
            response["channel"] = channel;
            response["status"] = "unsubscribed";

            //ec = glz::write_json(response, response_json);
            ws_conn_ptr->send(glz::write_json(response).value_or(""));
            logger.debugf("Client unsubscribed from: {}", channel);
        }
        else if (msg_type == "ping")
        {
            glz::generic pong_response;
            //std::string pong_response_json;

            pong_response["type"] = "pong";
            pong_response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
            
            //ec = glz::write_json(pong_response, pong_response_json);
            ws_conn_ptr->send(glz::write_json(pong_response).value_or(""));
        }
        else
        {
            logger.debugf("Unknown message type: {}", msg_type);
        }
    }
    catch (const std::exception& e)
    {
        logger.errorf("WebSocket message handler exception: {}", e.what());
        glz::generic error_response;
        //std::string error_response_json;

        error_response["type"] = "error";
        error_response["message"] = "Server error";

        //auto ec = glz::write_json(error_response, error_response_json);
        ws_conn_ptr->send(glz::write_json(error_response).value_or(""));
    }
}


void InzyghtWebSocketController::handleConnectionClosed(const WebSocketConnectionPtr& ws_conn_ptr)
{
    Logger::instance().infof("WebSocket client disconnected: {}", ws_conn_ptr->peerAddr().toIpPort());
    EventBroadcaster::instance().unregister_client(ws_conn_ptr);
}
