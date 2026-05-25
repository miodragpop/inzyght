#include "PostgresCopy.h"
#include "PostgresPool.h"
#include <libpq-fe.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include "../Logger.h"

namespace {

// Indexer-specific tuning for COPY-only sessions. synchronous_commit=off lets
// Postgres report COMMIT-success before fsync'ing WAL, which is the dominant
// per-batch cost. On power loss we can lose the last few seconds of writes —
// fine here because the indexer re-syncs missing blocks from RPC on restart.
void tune_copy_connection(PGconn* conn)
{
    if (!conn || PQstatus(conn) != CONNECTION_OK) return;
    PGresult* res = PQexec(conn, "SET synchronous_commit = off");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        Logger::instance().warnf("COPY pool: failed to set synchronous_commit=off: {}", PQerrorMessage(conn));
    }
    if (res) PQclear(res);
}

}  // namespace

/**
 * @brief Connection pool for dedicated COPY operations
 * Separate from Drogon's ORM pool to avoid blocking other database operations
 */
class CopyConnectionPool
{
public:
    static CopyConnectionPool& instance()
    {
        static CopyConnectionPool pool;
        return pool;
    }

    void initialize(const std::string& conn_str, int pool_size)
    {
        std::scoped_lock<std::mutex> lock(mutex_);

        // statement_timeout is intentionally NOT set on COPY connections — a
        // large COPY into "transactions" can legitimately exceed the default
        // here. Keepalives still apply.
        conn_str_ = augment_conn_str_with_keepalives(conn_str);
        pool_size_ = pool_size;

        // Create initial connections
        for (int i = 0; i < pool_size; ++i)
        {
            PGconn* conn = PQconnectdb(conn_str_.c_str());
            if (!conn)
            {
                Logger::instance().error(std::string("Failed to create COPY connection ") + std::to_string(i));
                continue;
            }

            if (PQstatus(conn) != CONNECTION_OK)
            {
                Logger::instance().error(std::string("COPY connection ") + std::to_string(i) + " failed: " + PQerrorMessage(conn));
                PQfinish(conn);
                continue;
            }

            harden_pg_connection(conn, /*statement_timeout_ms=*/0);
            tune_copy_connection(conn);
            available_connections_.push(conn);
            Logger::instance().debug(std::string("COPY connection ") + std::to_string(i) + " initialized");
        }

        if (available_connections_.empty())
        {
            throw std::runtime_error("Failed to initialize any COPY connections");
        }

        initialized_ = true;
    }

    PGconn* acquire(int timeout_ms = 1000)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!cond_var_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !available_connections_.empty(); }))
        {
            throw std::runtime_error("COPY connection pool timeout");
        }

        if (available_connections_.empty())
        {
            throw std::runtime_error("No COPY connections available");
        }

        PGconn* conn = available_connections_.front();
        available_connections_.pop();

        // Verify connection is still valid
        if (PQstatus(conn) != CONNECTION_OK)
        {
            Logger::instance().warn("COPY connection lost, reconnecting...");
            PQreset(conn);

            if (PQstatus(conn) != CONNECTION_OK)
            {
                PQfinish(conn);
                throw std::runtime_error("Failed to reconnect COPY connection");
            }

            harden_pg_connection(conn, /*statement_timeout_ms=*/0);
            tune_copy_connection(conn);
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
        Logger::instance().info("CopyConnectionPool: Shutdown initiated");

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

        Logger::instance().info("CopyConnectionPool: Shutdown complete");
    }

private:
    CopyConnectionPool() = default;

    mutable std::mutex mutex_;
    std::condition_variable cond_var_;
    std::queue<PGconn*> available_connections_;
    std::string conn_str_;
    int pool_size_ = 2;
    bool initialized_ = false;
    std::atomic<bool> shutdown_in_progress_{false};  // Non-blocking shutdown flag
};

/**
 * @brief RAII wrapper for connection lifecycle
 */
class ConnectionGuard
{
public:
    explicit ConnectionGuard(PGconn* conn) : conn_(conn) {}

    ~ConnectionGuard()
    {
        if (conn_)
        {
            try
            {
                CopyConnectionPool::instance().release(conn_);
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

    PGconn* get() const { return conn_; }

    PGconn* release()
    {
        PGconn* tmp = conn_;
        conn_ = nullptr;
        return tmp;
    }

private:
    PGconn* conn_ = nullptr;
};

void initialize_copy_pool(const std::string& connection_string, int pool_size)
{
    try
    {
        Logger::instance().info(std::string("Initializing COPY pool with connection string (pool size: ") + std::to_string(pool_size) + ")");
        CopyConnectionPool::instance().initialize(connection_string, pool_size);
        Logger::instance().info("COPY pool initialized successfully");
    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("Failed to initialize COPY pool: ") + e.what());
        throw;
    }
}

void shutdown_copy_pool()
{
    try
    {
        CopyConnectionPool::instance().shutdown();
        Logger::instance().info("COPY pool shutdown complete");
    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("Error shutting down COPY pool: ") + e.what());
    }
}

CopyResult execute_copy_internal(const std::string& table_name, const std::vector<std::string>& columns, const void* data, size_t data_size, bool binary)
{
    try {
        CopyConnectionPool& pool = CopyConnectionPool::instance();
        if (!pool.is_initialized())
        {
            throw std::runtime_error("COPY pool not initialized. Call initialize_copy_pool() first");
        }

        PGconn* raw_conn = pool.acquire();
        ConnectionGuard conn_guard(raw_conn);

        // Build COPY command
        std::string copy_command = std::format("COPY {} (", table_name);
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (i > 0) copy_command.append(", ");
            copy_command.append(columns[i]);
        }
        copy_command.append(binary ? ") FROM STDIN BINARY" : ") FROM STDIN");

        PGresult* res = PQexec(raw_conn, copy_command.c_str());

        if (!res)
        {
            throw std::runtime_error(std::format("COPY command failed: {}", PQerrorMessage(raw_conn)));
        }

        ExecStatusType status = PQresultStatus(res);
        PQclear(res);

        if (status != PGRES_COPY_IN)
        {
            throw std::runtime_error(std::format("COPY not in correct state: {}", PQresStatus(status)));
        }

        // Stream the data
        if (data_size > 0)
        {
            int put_result = PQputCopyData(raw_conn, static_cast<const char*>(data), static_cast<int>(data_size));

            if (put_result == -1)
            {
                throw std::runtime_error(std::format("PQputCopyData failed: {}", PQerrorMessage(raw_conn)));
            }

            if (put_result == 0)
            {
                Logger::instance().warn("PQputCopyData would block");
            }
        }

        // Finish the COPY
        int end_result = PQputCopyEnd(raw_conn, nullptr);
        if (end_result == -1)
        {
            throw std::runtime_error(std::format("PQputCopyEnd failed: {}", PQerrorMessage(raw_conn)));
        }

        // Get final result
        res = PQgetResult(raw_conn);
        if (!res)
        {
            throw std::runtime_error("No result from COPY");
        }

        status = PQresultStatus(res);

        if (status != PGRES_COMMAND_OK)
        {
            std::string err_msg = PQresultErrorMessage(res);
            PQclear(res);
            throw std::runtime_error(std::format("COPY command failed: {}", err_msg));
        }

        // Get affected rows from result
        char* tuples = PQcmdTuples(res);
        int64_t rows_affected = 0;

        if (tuples && tuples[0] != '\0')
        {
            try
            {
                rows_affected = std::stoll(tuples);
            }
            catch (const std::exception& e)
            {
                Logger::instance().warn(std::string("Failed to parse affected rows: ") + tuples);
            }
        }

        PQclear(res);

        return CopyResult{rows_affected, "COPY completed successfully", true};

    }
    catch (const std::exception& e)
    {
        Logger::instance().error(std::string("COPY execution failed: ") + e.what());
        throw;
    }
}

CopyResult execute_copy(const std::string& table_name,
                       const std::vector<std::string>& columns,
                       const std::string& data)
{
    return execute_copy_internal(table_name, columns, data.data(), data.size(), /*binary=*/false);
}

CopyResult execute_copy_binary(const std::string& table_name,
                             const std::vector<std::string>& columns,
                             const BinaryCopyBuffer& buf)
{
    return execute_copy_internal(table_name, columns, buf.data(), buf.size(), /*binary=*/true);
}
