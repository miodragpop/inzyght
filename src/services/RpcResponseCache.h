#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Process-wide cache for ycashd RPC responses keyed by (method, params).
//
// Between two blocks most RPC answers are byte-identical: block data looked
// up by hash, confirmed transactions, address balances/deltas — including
// their confirmations/nextblockhash fields — can only change when a new
// block arrives, and mempool-derived data only when a new transaction (or
// block) arrives. So instead of TTLs, entries are stamped with a generation
// counter that EventProcessor bumps on ZeroMQ block/tx events; a cached
// response stays valid exactly as long as the chain state it came from.
//
// A coarse max-age still applies to non-immutable entries as a safety net
// for the case where both ZeroMQ and the polling fallback are down and no
// invalidation events arrive at all.
class RpcResponseCache
{
    public:
        enum class Class
        {
            Immutable,        // pure function of the key (e.g. getblocksubsidy)
            BlockDependent,   // valid until the next block event
            MempoolDependent  // valid until the next transaction or block event
        };

        static RpcResponseCache& instance();

        // Returns the cached response, or nullopt if absent or stale.
        std::optional<std::string> get(const std::string& key);

        void put(const std::string& key, std::string value, Class cache_class);

        // Called by EventProcessor when the chain tip changes (also covers
        // reorgs — a reorg always surfaces as a block event). A new block
        // changes the mempool too, so both generations advance.
        void on_block_event();

        // Called by EventProcessor when a transaction enters the mempool.
        void on_transaction_event();

    private:
        RpcResponseCache() = default;
        ~RpcResponseCache() = default;

        struct Entry
        {
            std::string value;
            Class cache_class;
            uint64_t generation;       // generation at insert (unused for Immutable)
            uint64_t last_used;        // tick for LRU eviction
            std::chrono::steady_clock::time_point inserted_at;
        };

        static constexpr size_t kMaxEntries = 4096;
        static constexpr size_t kMaxBytes   = 64 * 1024 * 1024;
        static constexpr std::chrono::seconds kMaxAge {300};

        bool is_stale(const Entry& entry,
                      std::chrono::steady_clock::time_point now) const;
        void evict_for(size_t incoming_bytes);

        mutable std::mutex cache_mutex;
        std::unordered_map<std::string, Entry> entries;
        size_t total_bytes {0};
        uint64_t block_generation {0};
        uint64_t mempool_generation {0};
        uint64_t use_tick {0};
};
