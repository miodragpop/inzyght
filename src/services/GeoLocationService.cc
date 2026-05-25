#include "GeoLocationService.h"
#include "Logger.h"
#include "glaze/json/generic.hpp"
#include <curl/curl.h>
#include <sstream>

// ---------------------------------------------------------------------------
// ip-api.com response model
// ---------------------------------------------------------------------------
struct IpApiEntry
{
    std::string status;
    std::string country;
    std::string countryCode;
    std::string city;
    double lat {0.0};
    double lon {0.0};
    std::string query; // echoed IP
};

template <>
struct glz::meta<IpApiEntry>
{
    using T = IpApiEntry;
    static constexpr auto value = object(
        "status",      &T::status,
        "country",     &T::country,
        "countryCode", &T::countryCode,
        "city",        &T::city,
        "lat",         &T::lat,
        "lon",         &T::lon,
        "query",       &T::query
    );
};

// ---------------------------------------------------------------------------
// CURL write callback
// ---------------------------------------------------------------------------
static size_t geo_write_cb(void* contents, size_t size, size_t nmemb, std::string* out)
{
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// GeoLocationService
// ---------------------------------------------------------------------------

GeoLocationService& GeoLocationService::instance()
{
    static GeoLocationService inst;
    return inst;
}

std::string GeoLocationService::extract_ip(const std::string& addr)
{
    if (addr.empty()) return {};

    // IPv6: "[::1]:8233" → "::1"
    if (addr.front() == '[')
    {
        auto close = addr.find(']');
        if (close != std::string::npos)
            return addr.substr(1, close - 1);
        return addr;
    }

    // IPv4: "1.2.3.4:8233" → "1.2.3.4"
    auto colon = addr.rfind(':');
    if (colon != std::string::npos)
        return addr.substr(0, colon);

    return addr;
}

std::vector<GeoData> GeoLocationService::geolocate(const std::vector<std::string>& ips)
{
    // Collect IPs missing from cache
    std::vector<std::string> missing;
    {
        std::scoped_lock lock(cache_mutex);
        for (const auto& ip : ips)
            if (cache.find(ip) == cache.end())
                missing.push_back(ip);
    }

    if (!missing.empty())
        fetch_batch(missing);

    // Build result in input order
    std::vector<GeoData> result;
    result.reserve(ips.size());
    {
        std::scoped_lock lock(cache_mutex);
        for (const auto& ip : ips)
        {
            auto it = cache.find(ip);
            if (it != cache.end())
                result.push_back(it->second);
            else
            {
                GeoData d;
                d.ip = ip;
                result.push_back(d);
            }
        }
    }
    return result;
}

void GeoLocationService::fetch_batch(const std::vector<std::string>& ips)
{
    Logger& logger = Logger::instance();

    // Build JSON array: [{"query":"1.2.3.4","fields":"country,countryCode,city,lat,lon,query"}, ...]
    std::string body = "[";
    for (size_t i = 0; i < ips.size(); ++i)
    {
        if (i > 0) body += ',';
        body += R"({"query":")" + ips[i] + R"(","fields":"status,country,countryCode,city,lat,lon,query"})";
    }
    body += ']';

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        logger.error("GeoLocationService: curl_easy_init failed");
        return;
    }

    std::string response;
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "http://ip-api.com/batch");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, geo_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        logger.errorf("GeoLocationService: curl error: {}", curl_easy_strerror(res));
        return;
    }

    std::vector<IpApiEntry> entries;
    auto ec = glz::read_json(entries, response);
    if (ec)
    {
        logger.errorf("GeoLocationService: parse error: {}", ec.custom_error_message);
        return;
    }

    std::scoped_lock lock(cache_mutex);
    for (const auto& e : entries)
    {
        GeoData d;
        d.ip          = e.query;
        d.country     = e.country;
        d.countryCode = e.countryCode;
        d.city        = e.city;
        d.lat         = e.lat;
        d.lon         = e.lon;
        d.valid       = (e.status == "success");
        cache[d.ip]   = d;
    }

    logger.debugf("GeoLocationService: cached {} IPs", entries.size());
}
