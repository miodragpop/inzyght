#pragma once

#include <drogon/HttpFilter.h>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>
#include <string>

using namespace drogon;

// ── Shared sliding-window counter ─────────────────────────────────────────────
// Tracks request timestamps per IP. Thread-safe.
//
// The per-IP map is bounded by opportunistic eviction: a record is dropped as
// soon as its last request falls outside the window, and a periodic sweep
// removes any stragglers. Without this, an attacker rotating (or spoofing) the
// source IP would grow the map without limit — a memory-exhaustion DoS in its
// own right.
class SlidingWindowCounter
{
public:
    // Returns true if the request is allowed, false if rate exceeded.
    bool check(const std::string& ip, int max_requests, int window_seconds);

private:
    struct IpRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };

    // Remove IPs whose newest timestamp is older than `cutoff`. Caller holds mutex_.
    void evict_stale(std::chrono::steady_clock::time_point cutoff);

    std::mutex mutex_;
    std::unordered_map<std::string, IpRecord> clients_;
    std::chrono::steady_clock::time_point last_sweep_{};
};

// ── Strict limiter for expensive address endpoints ────────────────────────────
// Default: 20 requests / 60 seconds per IP
class AddressRateLimiter : public HttpFilter<AddressRateLimiter>
{
public:
    void doFilter(const HttpRequestPtr& req,
                  FilterCallback&&      fcb,
                  FilterChainCallback&& fccb) override;
};

// ── General limiter for all other API endpoints ───────────────────────────────
// Default: 120 requests / 60 seconds per IP
class ApiRateLimiter : public HttpFilter<ApiRateLimiter>
{
public:
    void doFilter(const HttpRequestPtr& req,
                  FilterCallback&&      fcb,
                  FilterChainCallback&& fccb) override;
};
