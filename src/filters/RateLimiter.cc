#include "RateLimiter.h"
#include "ConfigManager.h"
#include "Logger.h"
#include <drogon/HttpResponse.h>
#include <sstream>
#include <vector>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Determine the IP to rate-limit on.
//
// X-Forwarded-For is attacker-controlled unless a trusted proxy set it, so we
// only honour it when the *real* TCP peer is a configured trusted proxy. For a
// direct-to-internet deployment (no reverse proxy) trust_proxy must be false:
// we then always use the genuine TCP peer address, which a client cannot spoof.
//
//   [rate_limit]
//   trust_proxy   = false              ; true only if a reverse proxy fronts us
//   trusted_proxies = 127.0.0.1,::1    ; peers allowed to set X-Forwarded-For
//
static std::string get_client_ip(const HttpRequestPtr& req)
{
    ConfigManager& cfg = ConfigManager::instance();

    const std::string peer = req->getPeerAddr().toIp();

    // Default: do NOT trust forwarding headers. Correct for direct HTTPS.
    const bool trust_proxy = cfg.get_string("rate_limit", "trust_proxy", "false") == "true";
    if (!trust_proxy)
        return peer;

    // Only honour X-Forwarded-For when the immediate peer is a trusted proxy.
    const std::string proxies =
        cfg.get_string("rate_limit", "trusted_proxies", "127.0.0.1,::1");

    bool peer_is_trusted = false;
    std::stringstream ss(proxies);
    std::string entry;
    while (std::getline(ss, entry, ','))
    {
        // trim surrounding whitespace
        size_t b = entry.find_first_not_of(" \t");
        size_t e = entry.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        if (entry.substr(b, e - b + 1) == peer) { peer_is_trusted = true; break; }
    }

    if (!peer_is_trusted)
        return peer;  // peer isn't an approved proxy → ignore its XFF claim

    const std::string& xff = req->getHeader("X-Forwarded-For");
    if (xff.empty())
        return peer;

    // Leftmost value = original client (the trusted proxy appends, not prepends).
    auto comma = xff.find(',');
    std::string client = (comma == std::string::npos) ? xff : xff.substr(0, comma);
    size_t b = client.find_first_not_of(" \t");
    size_t e = client.find_last_not_of(" \t");
    if (b == std::string::npos)
        return peer;
    return client.substr(b, e - b + 1);
}

static HttpResponsePtr rate_limited_response()
{
    auto resp = HttpResponse::newHttpResponse(k429TooManyRequests, CT_APPLICATION_JSON);
    resp->setBody(R"({"status":"error","message":"Too many requests — please slow down."})");
    resp->addHeader("Retry-After", "60");
    return resp;
}

// ── SlidingWindowCounter ──────────────────────────────────────────────────────

void SlidingWindowCounter::evict_stale(std::chrono::steady_clock::time_point cutoff)
{
    // Drop any IP whose most recent request is already outside the window.
    for (auto it = clients_.begin(); it != clients_.end(); )
    {
        const auto& ts = it->second.timestamps;
        if (ts.empty() || ts.back() < cutoff)
            it = clients_.erase(it);
        else
            ++it;
    }
}

bool SlidingWindowCounter::check(const std::string& ip, int max_requests, int window_seconds)
{
    auto now    = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(window_seconds);

    std::scoped_lock lock(mutex_);

    // Periodic global sweep so the map can't accumulate idle IPs indefinitely.
    // Runs at most once per window; cheap relative to the request rate.
    if (last_sweep_ == std::chrono::steady_clock::time_point{} ||
        now - last_sweep_ >= std::chrono::seconds(window_seconds))
    {
        evict_stale(cutoff);
        last_sweep_ = now;
    }

    auto& record = clients_[ip];

    // Evict timestamps outside the window. Empty records are reaped by the
    // periodic sweep above, so an idle IP doesn't linger past one window.
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
