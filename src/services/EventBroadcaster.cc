#include "EventBroadcaster.h"
#include "Logger.h"
#include "glaze/json/generic.hpp"


EventBroadcaster& EventBroadcaster::instance()
{
    static EventBroadcaster instance;
    return instance;
}


void EventBroadcaster::register_client(const WebSocketConnectionPtr& client)
{
    std::scoped_lock<std::mutex> lock(clients_mutex);
    connected_clients.insert(client);
    Logger::instance().debugf("Client registered, total clients: {}", connected_clients.size());
}


void EventBroadcaster::unregister_client(const WebSocketConnectionPtr& client)
{
    std::scoped_lock<std::mutex> lock(clients_mutex);
    connected_clients.erase(client);
    Logger::instance().debugf("Client unregistered, total clients: {}", connected_clients.size());
}


void EventBroadcaster::broadcast_network_update(const glz::generic& network_data)
{
    glz::generic message;
    message["type"] = "network";
    message["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    message["data"] = network_data;
    send_to_all_clients(message);
}


void EventBroadcaster::broadcast_block_update(const glz::generic::object_t& block_data)
{
    glz::generic message;
    message["type"] = "block";
    message["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    message["data"] = block_data;
    send_to_all_clients(message);
}


void EventBroadcaster::broadcast_transaction_update(const glz::generic& tx_data)
{
    glz::generic message;
    message["type"] = "transaction";
    message["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    message["data"] = tx_data;
    send_to_all_clients(message);
}


void EventBroadcaster::broadcast_message(const std::string& message_type, const glz::generic& data)
{
    glz::generic message;
    message["type"] = message_type;
    message["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    message["data"] = data;
    send_to_all_clients(message);
}


void EventBroadcaster::send_to_all_clients(const glz::generic& message)
{
    std::scoped_lock<std::mutex> lock(clients_mutex);

    std::string message_str = glz::write_json(message).value_or("");

    // Send to all connected clients
    auto it = connected_clients.begin();
    while (it != connected_clients.end())
    {
        auto client = *it;
        if (client && client->connected())
        {
            client->send(message_str);
            ++it;
        }
        else
        {
            // Remove disconnected clients
            it = connected_clients.erase(it);
        }
    }
}
