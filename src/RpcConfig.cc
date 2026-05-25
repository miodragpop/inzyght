#include "RpcConfig.h"

RpcConfig::Config RpcConfig::get_default()
{
    Config config;
    config.host = DEFAULT_HOST;
    config.port = DEFAULT_PORT;
    config.username = "";
    config.password = "";
    config.timeout = DEFAULT_TIMEOUT;
    config.ssl = DEFAULT_SSL;
    config.validate_certificate = false;
    config.pool_size = DEFAULT_POOL_SIZE;
    config.connection_timeout = 5.0;
    // PostgreSQL defaults
    config.postgresql_host = "127.0.0.1";
    config.postgresql_port = 5432;
    config.postgresql_database = "inzyght";
    config.postgresql_username = "inzyght";
    config.postgresql_password = "inzyght";
    // ZeroMQ defaults
    config.zmq_enabled = true;
    config.zmq_endpoint = "tcp://127.0.0.1:48332";
    config.zmq_polling_fallback_timeout = 300;
    config.zmq_polling_interval = 300;
    // Performance defaults
    config.block_batch_size = 25;  // Process 25 blocks per batch (balance performance and memory)
    config.queue_depth = 5;        // Max 5 batches queued (backpressure control)
    return config;
}

