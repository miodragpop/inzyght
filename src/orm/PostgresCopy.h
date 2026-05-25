#pragma once

#include <cstdint>
#include <cstring>
#include <netinet/in.h>  // htonl, htons
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Simple PostgreSQL COPY operation result
 */
struct CopyResult
{
    int64_t rows_affected = 0;
    std::string message;
    bool success = false;
};

// --------------------------------------------------------------------------
// Binary COPY encoder
// --------------------------------------------------------------------------
//
// PostgreSQL's COPY ... FROM STDIN BINARY format is faster than text on both
// the client (no decimal/hex formatting) and the server (no parsing). Wire
// format per row: int16 field-count, then per field: int32 length (-1=NULL)
// followed by the value in network byte order.
//
// Usage:
//   BinaryCopyBuffer buf;
//   buf.write_header();
//   for (...) {
//       buf.begin_row(/*field_count=*/3);
//       buf.write_int4(height);
//       buf.write_bytea(hash_ptr, 32);
//       buf.write_int8(timestamp);
//   }
//   buf.write_trailer();
//   execute_copy_binary("blocks", {"height","hash","timestamp"}, buf);
//
// All multi-byte values are written in network byte order. The buffer reuses
// its storage across batches if you call clear() between batches.

#if defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  define INZYGHT_HTONLL(x) OSSwapHostToBigInt64(x)
#elif defined(__linux__)
#  include <endian.h>
#  define INZYGHT_HTONLL(x) htobe64(x)
#else
#  error "Define INZYGHT_HTONLL for this platform"
#endif

class BinaryCopyBuffer
{
public:
    BinaryCopyBuffer() { data_.reserve(64 * 1024); }

    void clear() { data_.clear(); }
    const char* data() const { return data_.data(); }
    size_t      size() const { return data_.size(); }
    void        reserve(size_t bytes) { data_.reserve(bytes); }

    // 11-byte signature + 4-byte flags + 4-byte header extension length.
    void write_header()
    {
        static constexpr char kSignature[] = "PGCOPY\n\xff\r\n\0";
        data_.insert(data_.end(), kSignature, kSignature + 11);
        write_raw_uint32(0);  // flags
        write_raw_uint32(0);  // header extension area length
    }

    // 0xFFFF marks end-of-data.
    void write_trailer()
    {
        write_raw_uint16(static_cast<uint16_t>(-1));
    }

    void begin_row(uint16_t field_count) { write_raw_uint16(field_count); }

    void write_null() { write_raw_uint32(static_cast<uint32_t>(-1)); }

    void write_bool(bool v)
    {
        write_raw_uint32(1);
        data_.push_back(v ? '\x01' : '\x00');
    }

    void write_int2(int16_t v)
    {
        write_raw_uint32(2);
        write_raw_uint16(static_cast<uint16_t>(v));
    }

    void write_int4(int32_t v)
    {
        write_raw_uint32(4);
        write_raw_uint32(static_cast<uint32_t>(v));
    }

    void write_int8(int64_t v)
    {
        write_raw_uint32(8);
        write_raw_uint64(static_cast<uint64_t>(v));
    }

    void write_float8(double v)
    {
        static_assert(sizeof(double) == 8, "double must be 8 bytes");
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        write_raw_uint32(8);
        write_raw_uint64(bits);
    }

    void write_bytea(const void* bytes, size_t len)
    {
        write_raw_uint32(static_cast<uint32_t>(len));
        const char* p = static_cast<const char*>(bytes);
        data_.insert(data_.end(), p, p + len);
    }

    void write_text(std::string_view s)
    {
        write_raw_uint32(static_cast<uint32_t>(s.size()));
        data_.insert(data_.end(), s.begin(), s.end());
    }

    // Convenience: decode a 64-char lowercase-or-mixed-case hex string into
    // 32 raw bytes and write as a bytea field. No allocation.
    bool write_bytea_from_hex64(std::string_view hex)
    {
        if (hex.size() != 64) return false;
        write_raw_uint32(32);
        for (size_t i = 0; i < 32; ++i)
        {
            int hi = hex_nibble(hex[i * 2]);
            int lo = hex_nibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            data_.push_back(static_cast<char>((hi << 4) | lo));
        }
        return true;
    }

private:
    static int hex_nibble(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    void write_raw_uint16(uint16_t v)
    {
        uint16_t n = htons(v);
        const char* p = reinterpret_cast<const char*>(&n);
        data_.insert(data_.end(), p, p + 2);
    }

    void write_raw_uint32(uint32_t v)
    {
        uint32_t n = htonl(v);
        const char* p = reinterpret_cast<const char*>(&n);
        data_.insert(data_.end(), p, p + 4);
    }

    void write_raw_uint64(uint64_t v)
    {
        uint64_t n = INZYGHT_HTONLL(v);
        const char* p = reinterpret_cast<const char*>(&n);
        data_.insert(data_.end(), p, p + 8);
    }

    std::vector<char> data_;
};

/**
 * @brief Initialize PostgreSQL COPY connection pool
 * Should be called during application startup
 *
 * @param connection_string PostgreSQL connection string (libpq format)
 * @param pool_size Number of dedicated COPY connections (default: 4)
 * @throws std::runtime_error if initialization fails
 */
void initialize_copy_pool(const std::string& connection_string, int pool_size = 4);

/**
 * @brief Shutdown PostgreSQL COPY connection pool
 * Should be called during application shutdown
 */
void shutdown_copy_pool();

/**
 * @brief Execute a COPY FROM STDIN operation
 *
 * @param table_name Name of the target table
 * @param columns Vector of column names in order
 * @param data Tab-separated values with newline-separated rows (COPY format)
 * @return CopyResult with row count and status
 * @throws std::runtime_error on database errors
 *
 * Example:
 *   std::vector<std::string> columns = {"height", "hash", "timestamp"};
 *   std::string data = "1\tblock_hash_1\t1234567890\n2\tblock_hash_2\t1234567891\n";
 *   CopyResult result = execute_copy("blocks", columns, data);
 */
CopyResult execute_copy(const std::string& table_name, const std::vector<std::string>& columns, const std::string& data);

/**
 * @brief Execute a COPY FROM STDIN BINARY operation.
 *
 * Issues `COPY <table_name> (<columns...>) FROM STDIN BINARY` and streams
 * @p buf (which must already contain the full PGCOPY header, row data, and
 * trailer; build it via BinaryCopyBuffer).
 */
CopyResult execute_copy_binary(const std::string& table_name,
                             const std::vector<std::string>& columns,
                             const BinaryCopyBuffer& buf);