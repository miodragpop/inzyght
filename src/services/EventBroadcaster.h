#pragma once

#include <string>
#include <set>
#include <mutex>
#include <drogon/WebSocketConnection.h>
#include "glaze/json/generic.hpp"

using drogon::WebSocketConnectionPtr;

class EventBroadcaster
{
    public:
        // Singleton instance
        static EventBroadcaster& instance();

        // Register a WebSocket client connection
        void register_client(const WebSocketConnectionPtr& client);

        // Unregister a WebSocket client connection
        void unregister_client(const WebSocketConnectionPtr& client);

        // Broadcast network update to all clients
        void broadcast_network_update(const glz::generic& network_data);

        // Broadcast block update to all clients
        void broadcast_block_update(const glz::generic::object_t& block_data);

        // Broadcast transaction update to all clients
        void broadcast_transaction_update(const glz::generic& tx_data);

        // Broadcast a generic message
        void broadcast_message(const std::string& message_type, const glz::generic& data);

    private:
        EventBroadcaster() = default;
        ~EventBroadcaster() = default;

        // Helper to send message to all connected clients
        void send_to_all_clients(const glz::generic& message);

        // Client registry (thread-safe)
        std::set<WebSocketConnectionPtr> connected_clients;
        mutable std::mutex clients_mutex;
};
