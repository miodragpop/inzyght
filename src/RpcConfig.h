#pragma once

#include <string>

class RpcConfig
{
    public:
        struct Config
        {
            std::string host;
            int port;
            std::string username;
            std::string password;
            double timeout;
            bool ssl;
            bool validate_certificate;
            int pool_size;
            double connection_timeout;
            // ZeroMQ settings
            std::string zmq_endpoint;
            bool zmq_enabled;
            int zmq_polling_fallback_timeout;  // seconds
            int zmq_polling_interval;           // seconds
            // PostgreSQL settings
            std::string postgresql_host;
            int postgresql_port;
            std::string postgresql_database;
            std::string postgresql_username;
            std::string postgresql_password;
            // Indexer performance settings
            int block_batch_size;           // Number of blocks to process per batch (default: 25)
            int block_headers_batch_size;   // Number of block headers to process per batch (default: 100)
            int queue_depth;                // Max batches queued between producer and consumer (default: 5)
        };

        // Get default configuration
        static Config get_default();

    private:
        static constexpr const char* DEFAULT_HOST = "127.0.0.1";
        static constexpr int DEFAULT_PORT = 8232;
        static constexpr double DEFAULT_TIMEOUT = 10.0;
        static constexpr int DEFAULT_POOL_SIZE = 10;
        static constexpr bool DEFAULT_SSL = false;
};
