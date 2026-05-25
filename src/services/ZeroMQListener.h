#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "RpcConfig.h"

class ZeroMQListener {
public:
    struct Event {
        std::string type;           // "block" or "transaction"
        std::string data;           // Raw event data (hash)
        long timestamp;             // Event timestamp
    };

    // Singleton instance
    static ZeroMQListener& instance();

    // Initialize ZeroMQ listener
    bool initialize(const RpcConfig::Config& config);

    // Start listening for events (runs in background thread)
    void start();

    // Stop listening
    void stop();

    // Get next event from queue (non-blocking)
    bool pop_event(Event& event);

    // Check if listener is running
    bool is_running() const { return running; }

    // Reset inactivity timer (called when processing events)
    void reset_inactivity_timer();

    // Check if we're in fallback mode (no ZeroMQ events)
    bool is_in_fallback_mode() const;

    // Get time since last event (in seconds)
    long get_time_since_last_event() const;

    // Wait for block event with timeout, interruptible by stop signal
    // Returns true if block event received, false if timeout/stop
    bool wait_for_block_event(std::stop_token stop_token, int timeout_seconds);

    // Destructor
    ~ZeroMQListener();

private:
    ZeroMQListener();

    // ZeroMQ listener thread function
    void listener_thread_func(std::stop_token stop_token);

    // Helper to parse ZeroMQ event
    Event parse_zeromq_event(const std::string& topic, const std::string& data, const std::string& event_type = "");

    // Configuration
    RpcConfig::Config config;

    // Thread management
    std::jthread listener_thread;
    std::atomic<bool> running{false};

    // Event queue (thread-safe)
    std::queue<Event> event_queue;
    mutable std::mutex zmq_queue_mutex;

    // Inactivity tracking
    std::atomic<long> last_event_time_{0};
    std::atomic<bool> in_fallback_mode_{false};

    // Block event notification (for immediate wake-up on new blocks)
    mutable std::mutex block_event_mutex;
    std::condition_variable block_event_cv;
    std::atomic<bool> block_event_received{false};

    // ZeroMQ socket pointers (void* to avoid ZMQ header dependency in header)
    void* zmq_context;
    void* socket;

    // Reconnection state
    int reconnect_attempts;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;
};
