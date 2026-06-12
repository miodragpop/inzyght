#include "RpcResponseCache.h"

#include <algorithm>

RpcResponseCache& RpcResponseCache::instance()
{
    static RpcResponseCache instance;
    return instance;
}

bool RpcResponseCache::is_stale(const Entry& entry,
                                std::chrono::steady_clock::time_point now) const
{
    switch (entry.cache_class)
    {
        case Class::Immutable:
            return false;
        case Class::BlockDependent:
            if (entry.generation != block_generation)
                return true;
            break;
        case Class::MempoolDependent:
            if (entry.generation != mempool_generation)
                return true;
            break;
    }
    return (now - entry.inserted_at) > kMaxAge;
}

std::optional<std::string> RpcResponseCache::get(const std::string& key)
{
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock<std::mutex> lock(cache_mutex);

    auto it = entries.find(key);
    if (it == entries.end())
        return std::nullopt;

    if (is_stale(it->second, now))
    {
        total_bytes -= it->second.value.size();
        entries.erase(it);
        return std::nullopt;
    }

    it->second.last_used = ++use_tick;
    return it->second.value;
}

void RpcResponseCache::put(const std::string& key, std::string value, Class cache_class)
{
    // A value that alone exceeds the byte budget would just evict everything
    // else without ever being useful — don't cache it.
    if (value.size() > kMaxBytes)
        return;

    std::scoped_lock<std::mutex> lock(cache_mutex);

    auto it = entries.find(key);
    if (it != entries.end())
    {
        total_bytes -= it->second.value.size();
        entries.erase(it);
    }

    evict_for(value.size());

    Entry entry;
    entry.cache_class = cache_class;
    entry.generation  = (cache_class == Class::MempoolDependent) ? mempool_generation
                                                                 : block_generation;
    entry.last_used   = ++use_tick;
    entry.inserted_at = std::chrono::steady_clock::now();
    total_bytes += value.size();
    entry.value = std::move(value);
    entries.emplace(key, std::move(entry));
}

// Caller must hold cache_mutex.
void RpcResponseCache::evict_for(size_t incoming_bytes)
{
    const auto now = std::chrono::steady_clock::now();

    auto over_budget = [&] {
        return entries.size() >= kMaxEntries ||
               total_bytes + incoming_bytes > kMaxBytes;
    };

    if (!over_budget())
        return;

    // First pass: drop everything already invalidated by a generation bump.
    for (auto it = entries.begin(); it != entries.end();)
    {
        if (is_stale(it->second, now))
        {
            total_bytes -= it->second.value.size();
            it = entries.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Still over budget: evict least-recently-used entries one at a time.
    // O(n) per eviction, but only runs when the cache is genuinely full.
    while (over_budget() && !entries.empty())
    {
        auto lru = std::min_element(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) {
                return a.second.last_used < b.second.last_used;
            });
        total_bytes -= lru->second.value.size();
        entries.erase(lru);
    }
}

void RpcResponseCache::on_block_event()
{
    std::scoped_lock<std::mutex> lock(cache_mutex);
    ++block_generation;
    ++mempool_generation;
}

void RpcResponseCache::on_transaction_event()
{
    std::scoped_lock<std::mutex> lock(cache_mutex);
    ++mempool_generation;
}
