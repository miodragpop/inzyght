#include "PostgresPool.h"
#include <libpq-fe.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include "../Logger.h"

void harden_pg_connection(PGconn* conn, int statement_timeout_ms)
{
    if (!conn || PQstatus(conn) != CONNECTION_OK) return;

    // Server-side cap on any single statement. Prevents the client from blocking
    // forever in PQgetResult on a wedged query.
    if (statement_timeout_ms > 0)
    {
        std::string sql = "SET statement_timeout = " + std::to_string(statement_timeout_ms);
        PGresult* res = PQexec(conn, sql.c_str());
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            Logger::instance().warnf("PG: failed to set statement_timeout: {}", PQerrorMessage(conn));
        }
        if (res) PQclear(res);
    }

    // NOTE: TCP keepalive parameters (keepalives_idle, keepalives_interval,
    // keepalives_count) are libpq connection-string parameters, not server
    // GUCs — they cannot be applied here. They are appended to the connection
    // string by augment_conn_str_with_keepalives() before PQconnectdb().
}

// Append libpq keepalive parameters to a connection string if not already present.
// Defaults make a silently-dead TCP socket observable in ~3 minutes.
std::string augment_conn_str_with_keepalives(const std::string& conn_str)
{
    std::string out = conn_str;
    auto append_if_missing = [&](const char* key, const char* value) {
        if (out.find(key) == std::string::npos)
        {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            out.append(key).push_back('=');
            out.append(value);
        }
    };
    append_if_missing("keepalives", "1");
    append_if_missing("keepalives_idle", "60");
    append_if_missing("keepalives_interval", "30");
    append_if_missing("keepalives_count", "3");
    append_if_missing("connect_timeout", "10");
    return out;
}

/**
 * @brief Connection pool for general-purpose database queries
 * Separate pool from COPY operations to allow concurrent access from multiple threads
 * Each thread acquires a unique connection, eliminating SSL/threading issues with libpq
 */
class QueryConnectionPool
{
public:
    static QueryConnectionPool& instance()
    {
        static QueryConnectionPool pool;
        return pool;
    }

    void initialize(const std::string& conn_str, int pool_size)
    {
        std::scoped_lock<std::mutex> lock(mutex_);

        conn_str_ = augment_conn_str_with_keepalives(conn_str);
        pool_size_ = pool_size;

        // Create initial connections
        for (int i = 0; i < pool_size; ++i)
        {
            PGconn* conn = PQconnectdb(conn_str_.c_str());
            if (!conn)
            {
                Logger::instance().error(std::string("Failed to create query connection ") + std::to_string(i));
                continue;
            }

            if (PQstatus(conn) != CONNECTION_OK)
            {
                Logger::instance().error(std::string("Query connection ") + std::to_string(i) + " failed: " + PQerrorMessage(conn));
                PQfinish(conn);
                continue;
            }

            harden_pg_connection(conn);
            available_connections_.push(conn);
            Logger::instance().debug(std::string("Query connection ") + std::to_string(i) + " initialized");
        }

        if (available_connections_.empty())
        {
            throw std::runtime_error("Failed to initialize any query connections");
        }

        initialized_ = true;
    }

    PGconn* acquire(int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!cond_var_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !available_connections_.empty(); }))
        {
            throw std::runtime_error("Query connection pool timeout");
        }

        if (available_connections_.empty())
        {
            throw std::runtime_error("No query connections available");
        }

        PGconn* conn = available_connections_.front();
        available_connections_.pop();

        // Verify connection is still valid
        if (PQstatus(conn) != CONNECTION_OK)
        {
            Logger::instance().warn("Query connection lost, reconnecting...");
            PQreset(conn);

            if (PQstatus(conn) != CONNECTION_OK)
            {
                PQfinish(conn);
                throw std::runtime_error("Failed to reconnect query connection");
            }

            harden_pg_connection(conn);
        }

        return conn;
    }

    void release(PGconn* conn)
    {
        // Fast path: If shutdown is in progress, immediately clean up without acquiring mutex
        if (shutdown_in_progress_.load(std::memory_order_acquire))
        {
            if (conn)
            {
                PQfinish(conn);  // Destroy connection directly, no pool return
            }
            return;  // Exit immediately, no blocking
        }

        // Normal path: Return to pool
        std::scoped_lock<std::mutex> lock(mutex_);

        if (!initialized_)
        {
            // Pool is already shut down, clean up the connection
            if (conn)
            {
                PQfinish(conn);
            }
            return;
        }

        if (conn)
        {
            available_connections_.push(conn);
            cond_var_.notify_one();
        }
    }

    bool is_initialized() const
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        return initialized_;
    }

    void shutdown()
    {
        Logger::instance().info("QueryConnectionPool: Shutdown initiated");

        // Set the shutdown flag FIRST (makes release() non-blocking)
        shutdown_in_progress_.store(true, std::memory_order_release);

        // Now acquire mutex and clean up pool resources
        std::scoped_lock<std::mutex> lock(mutex_);

        while (!available_connections_.empty())
        {
            PGconn* conn = available_connections_.front();
            available_connections_.pop();
            if (conn)
            {
                PQfinish(conn);
            }
        }

        initialized_ = false;

        Logger::instance().info("QueryConnectionPool: Shutdown complete");
    }

private:
    QueryConnectionPool() = default;

    mutable std::mutex mutex_;
    std::condition_variable cond_var_;
    std::queue<PGconn*> available_connections_;
    std::string conn_str_;
    int pool_size_ = 2;
    bool initialized_ = false;
    std::atomic<bool> shutdown_in_progress_{false};  // Non-blocking shutdown flag
};

// ============================================================================
// RAII wrapper for connection lifecycle
// ============================================================================

QueryConnectionGuard::QueryConnectionGuard(int timeout_ms)
{
    try
    {
        QueryConnectionPool& pool = QueryConnectionPool::instance();
        if (!pool.is_initialized())
        {
            throw std::runtime_error("Query pool not initialized. Call initialize_query_pool() first");
        }

        conn_ = pool.acquire(timeout_ms);
    }
    catch (const std::exception&)
    {
        // Ensure conn_ is null if acquisition failed
        conn_ = nullptr;
        throw;
    }
}

QueryConnectionGuard::~QueryConnectionGuard()
{
    if (conn_)
    {
        try
        {
            QueryConnectionPool::instance().release(conn_);
        }
        catch (const std::exception&)
        {
            // Suppress exceptions during cleanup
            // Pool may be shut down during static destruction
            if (conn_)
            {
                PQfinish(conn_);
            }
        }
    }
}


// ============================================================================
// Public API functions
// ============================================================================

void initialize_query_pool(const std::string& connection_string, int pool_size)
{
    try
    {
        Logger::instance().info(std::string("Initializing query pool with connection string (pool size: ") + std::to_string(pool_size) + ")");
        QueryConnectionPool::instance().initialize(connection_string, pool_size);
        Logger::instance().info("Query pool initialized successfully");
    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("Failed to initialize query pool: ") + e.what());
        throw;
    }
}

void shutdown_query_pool()
{
    try
    {
        QueryConnectionPool::instance().shutdown();
        Logger::instance().info("Query pool shutdown complete");
    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("Error shutting down query pool: ") + e.what());
    }
}
