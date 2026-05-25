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
class SlidingWindowCounter
{
public:
    // Returns true if the request is allowed, false if rate exceeded.
    bool check(const std::string& ip, int max_requests, int window_seconds);

private:
    struct IpRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, IpRecord> clients_;
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
