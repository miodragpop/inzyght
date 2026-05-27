#include "ZeroMQListener.h"
#include "Logger.h"
#include <chrono>
#include <cstring>

// ZeroMQ include guard
#ifdef HAVE_ZMQ
#include <zmq.h>
#else
// Stub types when ZeroMQ is not available
typedef void zmq_context_t;
#define ZMQ_SUB 0
#define ZMQ_POLLIN 0
#define ZMQ_LINGER 0
#define ZMQ_SUBSCRIBE 1
typedef struct {
    void* socket;
    int fd;
    short events;
    short revents;
} zmq_pollitem_t;
static inline void* zmq_ctx_new() { return nullptr; }
static inline void zmq_ctx_destroy(void*) {}
static inline void* zmq_socket(void*, int) { return nullptr; }
static inline int zmq_connect(void*, const char*) { return -1; }
static inline int zmq_close(void*) { return 0; }
static inline int zmq_setsockopt(void*, int, const void*, size_t) { return 0; }
static inline int zmq_poll(zmq_pollitem_t*, int, long) { return 0; }
static inline int zmq_recv(void*, void*, size_t, int) { return -1; }
#endif

ZeroMQListener& ZeroMQListener::instance() {
    static ZeroMQListener instance;
    return instance;
}

ZeroMQListener::ZeroMQListener()
    : zmq_context(nullptr), socket(nullptr), reconnect_attempts(0) {
    last_event_time_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

ZeroMQListener::~ZeroMQListener() {
    stop();
}

bool ZeroMQListener::initialize(const RpcConfig::Config& conf) {
    Logger& logger = Logger::instance();
    config = conf;

    if (!config.zmq_enabled) {
        logger.info("ZeroMQ listener disabled in configuration");
        return true;
    }

#ifndef HAVE_ZMQ
    logger.warn("ZeroMQ support not compiled in, falling back to polling only");
    in_fallback_mode_ = true;
    return true;
#endif

    logger.info("Initializing ZeroMQ listener");
    logger.debugf("ZeroMQ endpoint: {}", config.zmq_endpoint);

    // Create ZeroMQ context
    zmq_context = zmq_ctx_new();
    if (!zmq_context) {
        logger.error("Failed to create ZeroMQ context");
        return false;
    }

    return true;
}

void ZeroMQListener::start() {
    if (running) {
        return;
    }

    Logger& logger = Logger::instance();
    if (!config.zmq_enabled) {
        logger.info("ZeroMQ is disabled, not starting listener");
        return;
    }

#ifndef HAVE_ZMQ
    logger.info("ZeroMQ not available, polling fallback will be used");
    in_fallback_mode_ = true;
    return;
#endif

    logger.info("Starting ZeroMQ listener thread");
    running = true;
    listener_thread = std::jthread(&ZeroMQListener::listener_thread_func, this);
}

void ZeroMQListener::stop() {
    if (!running) {
        return;  // Already stopped
    }

    Logger& logger = Logger::instance();
    logger.info("Stopping ZeroMQ listener");

    listener_thread.request_stop();
    running = false;

    if (listener_thread.joinable()) {
        listener_thread.join();
    }

    // Clean up ZeroMQ resources
    if (socket) {
        zmq_close(socket);
        socket = nullptr;
    }
    if (zmq_context) {
        zmq_ctx_destroy(zmq_context);
        zmq_context = nullptr;
    }

    logger.info("ZeroMQ listener stopped");
}

bool ZeroMQListener::pop_event(Event& event) {
    std::scoped_lock<std::mutex> lock(zmq_queue_mutex);
    if (event_queue.empty()) {
        return false;
    }
    event = event_queue.front();
    event_queue.pop();
    return true;
}

void ZeroMQListener::inject_event(const std::string& type, const std::string& data) {
    Event event;
    event.type = type;
    event.data = data;
    event.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    {
        std::scoped_lock<std::mutex> lock(zmq_queue_mutex);
        event_queue.push(event);
        if (event_queue.size() > 1000) {
            event_queue.pop();
        }
    }
    if (type == "block") {
        block_event_received = true;
        block_event_cv.notify_all();
    }
}

void ZeroMQListener::reset_inactivity_timer() {
    last_event_time_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    in_fallback_mode_ = false;
}

bool ZeroMQListener::is_in_fallback_mode() const {
    return in_fallback_mode_;
}

long ZeroMQListener::get_time_since_last_event() const {
    long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    return now - last_event_time_;
}

bool ZeroMQListener::wait_for_block_event(std::stop_token stop_token, int timeout_seconds) {
    // Wait for block event with timeout, interruptible by stop signal
    // Polls with 250ms intervals to allow quick shutdown
    // Returns true if block event received, false if timeout or stop requested

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        // Check for stop signal before waiting
        if (stop_token.stop_requested()) {
            return false;
        }

        std::unique_lock<std::mutex> lock(block_event_mutex);
        block_event_received = false;

        // Wait with short timeout (250ms) so we can check stop signal frequently
        bool notified = block_event_cv.wait_for(lock, std::chrono::milliseconds(250),
                                               [this] { return block_event_received.load(); });

        if (notified) {
            return true;  // Block event received
        }
    }

    return false;  // Timeout elapsed
}

void ZeroMQListener::listener_thread_func(std::stop_token stop_token) {
    Logger& logger = Logger::instance();
    logger.info("ZeroMQ listener thread started");

    while (!stop_token.stop_requested()) {
        try {
            // Create ZeroMQ socket
            socket = zmq_socket(zmq_context, ZMQ_SUB);

            if (!socket) {
                logger.error("Failed to create ZeroMQ socket");
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            // Subscribe to all messages
            zmq_setsockopt(socket, ZMQ_SUBSCRIBE, "", 0);

            // Set connection timeout
            int linger = 0;
            zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));

            // Connect to endpoint
            if (zmq_connect(socket, config.zmq_endpoint.c_str()) != 0) {
                logger.errorf("Failed to connect to endpoint: {}", config.zmq_endpoint);
                zmq_close(socket);
                socket = nullptr;
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            logger.infof("Connected to ZeroMQ endpoint: {}", config.zmq_endpoint);
            reconnect_attempts = 0;
            in_fallback_mode_ = false;
            reset_inactivity_timer();

            // Poll socket for events
            zmq_pollitem_t items[] = {
                { socket, 0, ZMQ_POLLIN, 0 }
            };

            while (!stop_token.stop_requested()) {
                // Poll with 100ms timeout for responsive shutdown
                int poll_result = zmq_poll(items, 1, 100);

                if (poll_result == -1) {
                    logger.error("ZeroMQ poll error");
                    break;
                }

                // Check socket for messages
                if (items[0].revents & ZMQ_POLLIN) {
                    // Receive first part (might be envelope/sequence)
                    char part1[256];
                    int part1_size = zmq_recv(socket, part1, 255, 0);
                    if (part1_size != -1) {
                        // Check if there are more parts (multi-part message)
                        int more = 0;
                        size_t more_size = sizeof(more);
                        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);

                        if (more) {
                            // Multi-part message: first part is envelope, rest are topic/data
                            char part2[256];
                            int part2_size = zmq_recv(socket, part2, 255, 0);

                            zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
                            if (more && part2_size != -1) {
                                char part3[256];
                                int part3_size = zmq_recv(socket, part3, 255, 0);

                                if (part3_size != -1) {
                                    logger.debugf("Event envelope: command={}, hash_size={}, seq_size={}",
                                                            std::string(part1, part1_size), part2_size, part3_size);

                                    // Ycash format: part1=command string, part2=32-byte hash, part3=4-byte sequence
                                    // parse_zeromq_event will determine event type from content
                                    Event event = parse_zeromq_event(std::string(part1, part1_size),
                                                                  std::string(part2, part2_size),
                                                                  "");
                                    {
                                        std::scoped_lock<std::mutex> lock(zmq_queue_mutex);
                                        event_queue.push(event);
                                        if (event_queue.size() > 1000) {
                                            event_queue.pop();
                                        }
                                    }
                                    // Notify all waiters of block events for immediate wake-up
                                    if (event.type == "block") {
                                        block_event_received = true;
                                        block_event_cv.notify_all();
                                    }
                                    reset_inactivity_timer();
                                    logger.debugf("{} event received: {}", event.type == "block" ? "Block" : (event.type == "transaction" ? "Transaction" : "Unknown"), event.data);
                                }
                            }
                        } else {
                            // Single part message (standard format)
                            part1[part1_size] = '\0';
                            char data[256];
                            int data_size = zmq_recv(socket, data, 255, 0);
                            if (data_size != -1) {
                                data[data_size] = '\0';

                                Event event = parse_zeromq_event(std::string(part1, part1_size),
                                                              std::string(data, data_size),
                                                              "");
                                {
                                    std::scoped_lock<std::mutex> lock(zmq_queue_mutex);
                                    event_queue.push(event);
                                    if (event_queue.size() > 1000) {
                                        event_queue.pop();
                                    }
                                }
                                // Notify all waiters of block events for immediate wake-up
                                if (event.type == "block") {
                                    block_event_received = true;
                                    block_event_cv.notify_all();
                                }
                                reset_inactivity_timer();
                                logger.debugf("SINGLE PART event received: {}", event.data);
                            }
                        }
                    }
                }

                // Check for fallback mode trigger
                if (get_time_since_last_event() > config.zmq_polling_fallback_timeout) {
                    if (!in_fallback_mode_) {
                        logger.warnf("No ZeroMQ events for {} seconds, entering fallback mode",
                                              config.zmq_polling_fallback_timeout);
                        in_fallback_mode_ = true;
                    }
                }
            }

        } catch (const std::exception& e) {
            Logger& logger = Logger::instance();
            logger.errorf("ZeroMQ listener exception: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        if (!stop_token.stop_requested()) {
            // Clean up and prepare for reconnection
            if (socket) {
                zmq_close(socket);
                socket = nullptr;
            }

            if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                reconnect_attempts++;
                int wait_time = 1 << (reconnect_attempts - 1); // Exponential backoff
                logger.infof("Reconnecting ZeroMQ (attempt {}) in {} seconds",
                                       reconnect_attempts, wait_time);
                std::this_thread::sleep_for(std::chrono::seconds(wait_time));
            } else {
                logger.warnf("ZeroMQ reconnection failed after {} attempts, entering fallback mode",
                                       MAX_RECONNECT_ATTEMPTS);
                in_fallback_mode_ = true;
                std::this_thread::sleep_for(std::chrono::seconds(30)); // Wait longer before retrying
            }
        }
    }

    logger.info("ZeroMQ listener thread ended");
}

ZeroMQListener::Event ZeroMQListener::parse_zeromq_event(const std::string& topic, const std::string& data, const std::string& event_type) {
    Event event;
    event.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();

    Logger& logger = Logger::instance();

    // Convert to hex for debugging
    std::string topic_hex = "";
    for (unsigned char c : topic) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        topic_hex += buf;
    }
    std::string data_hex = "";
    for (unsigned char c : data) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        data_hex += buf;
    }

    std::string hash_bytes;  // Will hold the hash in binary form

    // Determine event type and extract hash based on message format
    // Ycash format: part1=command string ("hashblock"/"hashtx"), part2=32-byte hash, part3=4-byte sequence
    // We receive part1 as topic parameter, part2 as data parameter
    if (topic == "hashblock") {
        event.type = "block";
        hash_bytes = data;
    } else if (topic == "hashtx") {
        event.type = "transaction";
        hash_bytes = data;
    }
    // Legacy Format 2: topic is 4-byte value, data="hashblock"/"hashtx" (6 bytes)
    else if (data == "hashblock") {
        event.type = "block";
        hash_bytes = topic;  // Hash is in topic field
    } else if (data == "hashtx") {
        event.type = "transaction";
        hash_bytes = topic;  // Hash is in topic field
    }
    // Legacy Format 3: topic=32-byte hash, data=4-byte little-endian sequence number
    // This shouldn't occur with corrected socket handlers but kept for compatibility
    else if (topic.length() == 32 && data.length() == 4) {
        // Use provided event type if available, otherwise infer from context
        if (!event_type.empty()) {
            event.type = event_type;
        } else {
            event.type = "block";  // Default to block
        }
        hash_bytes = topic;

        // Extract and log sequence number
        uint32_t seq = 0;
        for (int i = 0; i < 4; i++) {
            seq |= (unsigned char)data[i] << (i * 8);
        }
        logger.debugf("ZeroMQ legacy format - seq: {}, hash_hex: {}..., type: {}",
                                seq, topic_hex.substr(0, 16), (event_type.empty() ? "inferred" : event_type));
    } else {
        event.type = "unknown";
        logger.debugf("ZeroMQ unexpected format - topic_hex: {} (len:{}), data_hex: {} (len:{})",
                                topic_hex, topic.length(), data_hex, data.length());
    }

    // Convert hash to hex string
    event.data = "";
    for (unsigned char c : hash_bytes) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        event.data += buf;
    }

    // Log formatted event for easier reading
    if (event.type == "block") {
        logger.debugf("Parsed ZeroMQ block event: hash={}", event.data);
    } else if (event.type == "transaction") {
        logger.debugf("Parsed ZeroMQ transaction event: hash={}", event.data);
    } else {
        logger.debugf("Parsed ZeroMQ unknown event: type={}, topic_hex={}, data_hex={}",
                                event.type,
                                (topic_hex.length() > 16 ? topic_hex.substr(0, 16) + "..." : topic_hex),
                                (data_hex.length() > 16 ? data_hex.substr(0, 16) + "..." : data_hex));
    }

    return event;
}
