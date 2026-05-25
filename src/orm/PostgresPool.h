#pragma once

#include <libpq-fe.h>
#include <string>

/**
 * @brief Apply hardening to a freshly-opened libpq connection.
 *
 * Sets a server-side statement_timeout and enables TCP keepalives so that a
 * silently-broken connection surfaces as an error within minutes instead of
 * blocking recv() for hours on the kernel default keepalive interval.
 *
 * Safe to call multiple times (e.g. after PQreset). Logs warnings on failure
 * but does not throw — the caller decides whether the connection is usable.
 *
 * @param conn  Open libpq connection (must be CONNECTION_OK).
 * @param statement_timeout_ms  Server-side per-statement cap, in milliseconds. 0 disables.
 */
void harden_pg_connection(PGconn* conn, int statement_timeout_ms = 120000);

/**
 * @brief Return @p conn_str with libpq keepalive params and a connect_timeout
 * appended (only if not already present). Use this on every connection string
 * before passing it to PQconnectdb so silently-dead TCP sockets surface within
 * minutes rather than the kernel default of ~2 hours.
 */
std::string augment_conn_str_with_keepalives(const std::string& conn_str);

/**
 * @brief Initialize PostgreSQL general-purpose connection pool
 * Should be called during application startup, after initialize_copy_pool()
 *
 * @param connection_string PostgreSQL connection string (libpq format)
 * @param pool_size Number of general-purpose connections (default: 4)
 * @throws std::runtime_error if initialization fails
 */
void initialize_query_pool(const std::string& connection_string, int pool_size = 4);

/**
 * @brief Shutdown PostgreSQL general-purpose connection pool
 * Should be called during application shutdown, before shutdown_copy_pool()
 */
void shutdown_query_pool();

/**
 * @brief RAII wrapper for acquiring a connection from the query pool
 *
 * Acquires a connection on construction and returns it to the pool on destruction.
 * This is the primary way to access connections from the query pool. It ensures
 * proper cleanup and prevents connection leaks.
 *
 * Example usage:
 *   {
 *       QueryConnectionGuard guard;
 *       PGresult* res = PQexec(guard.conn(), "SELECT * FROM table");
 *       if (res) {
 *           // process result
 *           PQclear(res);
 *       }
 *   } // guard destructor returns connection to pool
 */
class QueryConnectionGuard
{
public:
    /**
     * @brief Acquire a connection from the pool
     * @param timeout_ms Maximum time in milliseconds to wait for a connection
     * @throws std::runtime_error if pool is not initialized, timeout occurs, or connection is invalid
     */
    explicit QueryConnectionGuard(int timeout_ms = 5000);

    /**
     * @brief Release connection back to pool
     */
    ~QueryConnectionGuard();

    /**
     * @brief Get the underlying PostgreSQL connection handle
     * @return PGconn* pointer, valid for the lifetime of this guard object
     */
    PGconn* conn() const { return conn_; }

    // Non-copyable, non-movable: this guard is only used as a stack-local RAII.
    QueryConnectionGuard(const QueryConnectionGuard&) = delete;
    QueryConnectionGuard& operator=(const QueryConnectionGuard&) = delete;
    QueryConnectionGuard(QueryConnectionGuard&&) = delete;
    QueryConnectionGuard& operator=(QueryConnectionGuard&&) = delete;

private:
    PGconn* conn_ = nullptr;
};
