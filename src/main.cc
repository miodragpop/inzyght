#include <drogon/drogon.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <latch>
#include <thread>
#include <format>
#include "ConfigManager.h"
#include "Logger.h"
#include "Version.h"
#include "ycash_rpc_client.h"
#include "services/ZeroMQListener.h"
#include "services/EventProcessor.h"
#include "services/PollingFallback.h"
//#include "services/EventBroadcaster.h"
#include "services/BlockIndexer.h"
#include "services/BlockchainState.h"
#include "orm/PostgresCopy.h"
#include "orm/PostgresPool.h"

// Global flag for signal handling
static std::atomic<bool> shutdown_requested{false};

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        shutdown_requested = true;
        drogon::app().quit();
    }
}

int main(int argc, char* argv[]) {
    // --version / -V: print and exit before doing anything else (no log file,
    // no config, no side effects).
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-V") {
            std::cout << "inzyght " << inzyght::k_version << std::endl;
            return 0;
        }
    }

    try {
        // Register signal handlers for graceful shutdown
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // Initialize logger first (before loading config)
        Logger& logger = Logger::instance();
        logger.init("./inzyght.log", LogLevel::DEBUG);

        logger.infof("=== Inzyght {} starting ===", inzyght::k_version);

        // Load application configuration from inzyght.conf
        ConfigManager& config = ConfigManager::instance();
        if (!config.load_config("./inzyght.conf")) {
            logger.error("Failed to load inzyght.conf");
            return 1;
        }

        // Log loaded configuration
        logger.info("Configuration Loaded");

        // Set log level from config
        std::string log_level_str = config.get_log_level();
        if (log_level_str == "DEBUG") {
            logger.set_log_level(LogLevel::DEBUG);
        } else if (log_level_str == "INFO") {
            logger.set_log_level(LogLevel::INFO);
        } else if (log_level_str == "WARN") {
            logger.set_log_level(LogLevel::WARN);
        } else if (log_level_str == "ERROR") {
            logger.set_log_level(LogLevel::ERROR);
        }
        logger.debugf("Log level set to: {}", log_level_str);

        logger.debugf("Server: {}:{}", config.get_server_host(), config.get_server_port());
        logger.debugf("Threads: {}", config.get_thread_count());
        logger.debugf("Database: {}", config.get_database_type());
        logger.debugf("RPC: {}:{}", config.get_rpc_config().host, config.get_rpc_config().port);

        // Get the application instance
        logger.info("Getting Drogon app instance...");
        auto& app = drogon::app();

        // Load Drogon configuration
        logger.info("Loading Drogon configuration...");
        app.loadConfigFile("./config/drogon_config.json");
        logger.info("Drogon configuration loaded");

        app.setCustom404Page(drogon::HttpResponse::newFileResponse("./public/404.html"));

        // Initialize COPY support for bulk operations
        // Build PostgreSQL connection string from config
        // Note: Try both "user" and "username" keys for compatibility
        std::string pg_user = config.get_string("postgresql", "user", "");
        if (pg_user.empty()) {
            pg_user = config.get_string("postgresql", "username", "postgres");
        }
        std::string pg_password = config.get_string("postgresql", "password", "");
        std::string pg_host = config.get_string("postgresql", "host", "localhost");
        int pg_port = config.get_int("postgresql", "port", 5432);
        std::string pg_database = config.get_string("postgresql", "database", "inzyght");

        // Build libpq connection string (key=value format)
        // Prefer Unix socket for localhost (faster than TCP when available)
        std::string pg_conn_str;
        if (pg_host == "localhost" || pg_host == "127.0.0.1") {
            // Try Unix socket first for better performance
            // DrogonCopyExtension will fall back to TCP if socket fails
            pg_conn_str = std::format("host=/var/run/postgresql port={} dbname={} user={}",
                                    pg_port, pg_database, pg_user);
            logger.debug("Using Unix socket (with TCP fallback) for PostgreSQL connection");
        } else {
            // Use TCP for remote hosts
            pg_conn_str = std::format("host={} port={} dbname={} user={}",
                                    pg_host, pg_port, pg_database, pg_user);
            logger.debug("Using TCP connection for PostgreSQL (remote host)");
        }
        if (!pg_password.empty()) {
            pg_conn_str += std::format(" password={}", pg_password);
        }

        logger.debugf("PostgreSQL connection string: {}", pg_conn_str);

        try {
            initialize_copy_pool(pg_conn_str, 4);
            logger.info("COPY pool initialized successfully with 4 connections");
        } catch (const std::exception& e) {
            logger.errorf("CRITICAL: Failed to initialize COPY pool: {}", e.what());
            logger.error("BlockIndexer requires COPY operations - cannot continue without PostgreSQL");
            return 1;
        }

        try {
            int query_pool_size = config.get_int("postgresql", "query_pool_size", 4);
            initialize_query_pool(pg_conn_str, query_pool_size);
            logger.infof("Query pool initialized successfully with {} connections", query_pool_size);
        } catch (const std::exception& e) {
            logger.errorf("CRITICAL: Failed to initialize query pool: {}", e.what());
            logger.error("BlockIndexer requires query pool - cannot continue without PostgreSQL");
            return 1;
        }

        // Configure from inzyght.conf
        std::string server_host = config.get_server_host();
        int server_port = config.get_server_port();
        int thread_count = config.get_thread_count();

        // Set HTTP listener address and port
        logger.debugf("Adding listener on {}:{}", server_host, server_port);
        app.addListener(server_host, server_port);

        // Set the number of threads
        app.setThreadNum(thread_count);

        logger.infof("Server listening on http://{}:{}", server_host, server_port);
        logger.infof("Worker threads: {}", thread_count);
        logger.info("=== Inzyght Ready ===");

        // Initialize RPC client singleton (with both main and adhoc curl backends)
        logger.info("Initializing RPC client...");
        if (!YcashRpcClient::initialize(config.get_rpc_config())) {
            logger.error("CRITICAL: Failed to initialize RPC client");
            return 1;
        }
        logger.info("RPC client initialized successfully");

        // Populate blockchain state cache before any ZeroMQ events arrive
        logger.info("Populating initial blockchain state cache...");
        BlockchainState::instance().update_state_on_block_event();
        logger.info("Initial blockchain state cache populated");

        // Get singleton references early
        ZeroMQListener& zmq_listener = ZeroMQListener::instance();
        EventProcessor& event_processor = EventProcessor::instance();
        PollingFallback& polling_fallback = PollingFallback::instance();
        BlockIndexer& block_indexer = BlockIndexer::instance();

        // Use std::latch for parallel service startup synchronization
        // Count: EventProcessor, PollingFallback, BlockIndexer (ZeroMQListener is optional)
        std::latch startup_ready(3);

        // Start services in parallel
        std::thread zmq_thread([&]() {
            logger.info("Initializing ZeroMQ event listener...");
            if (zmq_listener.initialize(config.get_rpc_config())) {
                zmq_listener.start();
                logger.info("ZeroMQ listener started");
            } else {
                logger.warn("Failed to initialize ZeroMQ, using polling fallback only");
            }
        });

        std::thread event_processor_thread([&]() {
            logger.info("Initializing event processor service...");
            event_processor.initialize(config.get_rpc_config());
            event_processor.start();
            logger.info("Event processor service started");
            startup_ready.count_down();
        });

        std::thread polling_fallback_thread([&]() {
            logger.info("Initializing polling fallback service...");
            polling_fallback.initialize(config.get_rpc_config());
            polling_fallback.start();
            logger.info("Polling fallback service started");
            startup_ready.count_down();
        });

        std::thread block_indexer_thread([&]() {
            logger.info("Initializing block indexer service...");
            if (block_indexer.initialize(config.get_rpc_config())) {
                block_indexer.start();
                logger.info("Block indexer service started - asynchronous historical sync in progress");
            } else {
                logger.warn("Failed to initialize block indexer - historical data will not be indexed");
            }
            startup_ready.count_down();
        });

        // Wait for all critical services to be ready
        logger.info("Waiting for all services to initialize...");
        startup_ready.wait();
        logger.info("All services initialized and ready");

        // ZeroMQ thread doesn't count in latch, so we need to ensure it's started
        zmq_thread.detach();

        // Detach other threads as they run independently
        event_processor_thread.detach();
        polling_fallback_thread.detach();
        block_indexer_thread.detach();

        // Run the application
        logger.info("Starting Drogon app.run()...");
        app.run();
        
        logger.info("=== Inzyght Shutting Down ===");
        logger.info("Stopping ZeroMQ listener...");
        zmq_listener.stop();
        logger.info("Stopping polling fallback...");
        polling_fallback.stop();        
        logger.info("Stopping event processor...");
        event_processor.stop();        
        logger.info("Stopping block indexer...");
        block_indexer.stop();
        logger.info("All services stopped cleanly");

        // Shutdown query pool
        logger.info("Shutting down query pool...");
        shutdown_query_pool();

        // Shutdown COPY pool
        logger.info("Shutting down COPY pool...");
        shutdown_copy_pool();

        logger.info("=== Inzyght shutdown: done ===");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << std::format("Fatal error: {}", e.what()) << std::endl;
        return 1;
    } catch (...) {
        // Fallback for non-std::exception types
        std::cerr << "Unknown fatal error occurred (non-standard exception)" << std::endl;
        return 1;
    }
}
