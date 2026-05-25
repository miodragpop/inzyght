#include "RateLimiter.h"
#include "ConfigManager.h"
#include "Logger.h"
#include <drogon/HttpResponse.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string get_client_ip(const HttpRequestPtr& req)
{
    // Respect X-Forwarded-For set by Apache reverse proxy
    const std::string& xff = req->getHeader("X-Forwarded-For");
    if (!xff.empty())
    {
        // Take only the first IP (leftmost = real client)
        auto comma = xff.find(',');
        return comma == std::string::npos ? xff : xff.substr(0, comma);
    }
    return req->getPeerAddr().toIp();
}

static HttpResponsePtr rate_limited_response()
{
    auto resp = HttpResponse::newHttpResponse(k429TooManyRequests, CT_APPLICATION_JSON);
    resp->setBody(R"({"status":"error","message":"Too many requests — please slow down."})");
    resp->addHeader("Retry-After", "60");
    return resp;
}

// ── SlidingWindowCounter ──────────────────────────────────────────────────────

bool SlidingWindowCounter::check(const std::string& ip, int max_requests, int window_seconds)
{
    auto now    = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(window_seconds);

    std::scoped_lock lock(mutex_);
    auto& record = clients_[ip];

    // Evict timestamps outside the window
    while (!record.timestamps.empty() && record.timestamps.front() < cutoff)
        record.timestamps.pop_front();

    if (static_cast<int>(record.timestamps.size()) >= max_requests)
        return false;

    record.timestamps.push_back(now);
    return true;
}

// ── AddressRateLimiter ────────────────────────────────────────────────────────

static SlidingWindowCounter address_counter;

void AddressRateLimiter::doFilter(const HttpRequestPtr& req,
                                  FilterCallback&&      fcb,
                                  FilterChainCallback&& fccb)
{
    ConfigManager& cfg = ConfigManager::instance();
    int max_req = cfg.get_int("rate_limit", "address_max_requests", 20);
    int window  = cfg.get_int("rate_limit", "address_window_seconds", 60);

    std::string ip = get_client_ip(req);

    if (!address_counter.check(ip, max_req, window))
    {
        Logger::instance().warnf("Rate limit exceeded (address) for IP: {}", ip);
        fcb(rate_limited_response());
        return;
    }

    fccb();
}

// ── ApiRateLimiter ────────────────────────────────────────────────────────────

static SlidingWindowCounter api_counter;

void ApiRateLimiter::doFilter(const HttpRequestPtr& req,
                              FilterCallback&&      fcb,
                              FilterChainCallback&& fccb)
{
    ConfigManager& cfg = ConfigManager::instance();
    int max_req = cfg.get_int("rate_limit", "api_max_requests", 120);
    int window  = cfg.get_int("rate_limit", "api_window_seconds", 60);

    std::string ip = get_client_ip(req);

    if (!api_counter.check(ip, max_req, window))
    {
        Logger::instance().warnf("Rate limit exceeded (api) for IP: {}", ip);
        fcb(rate_limited_response());
        return;
    }

    fccb();
}
