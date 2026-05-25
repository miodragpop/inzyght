#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>

struct GeoData
{
    std::string ip;
    std::string country;
    std::string countryCode;
    std::string city;
    double lat {0.0};
    double lon {0.0};
    bool valid {false};
};

class GeoLocationService
{
public:
    static GeoLocationService& instance();

    // Returns geo data for a list of IPs (stripped of port).
    // Results are cached permanently for the lifetime of the process.
    std::vector<GeoData> geolocate(const std::vector<std::string>& ips);

    // Strip port from "1.2.3.4:8233" or "[::1]:8233"
    static std::string extract_ip(const std::string& addr);

private:
    GeoLocationService() = default;

    // Fetch batch from ip-api.com for IPs not yet in cache
    void fetch_batch(const std::vector<std::string>& ips);

    mutable std::mutex cache_mutex;
    std::map<std::string, GeoData> cache;
};
