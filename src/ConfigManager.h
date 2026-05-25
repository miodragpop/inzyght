#pragma once

#include <string>
#include <map>
#include <memory>
#include <json/json.h>
#include "RpcConfig.h"

class ConfigManager {
public:
    // Load configuration from inzyght.conf
    static ConfigManager& instance();

    // Load configuration file
    bool load_config(const std::string& config_path);

    // Get RPC configuration
    RpcConfig::Config get_rpc_config() const;

    // Get server settings
    std::string get_server_host() const;
    int get_server_port() const;
    int get_thread_count() const;
    std::string get_log_level() const;

    // Get database settings
    std::string get_database_type() const;

    // Get raw value from any section
    std::string get_string(const std::string& section, const std::string& key,
                         const std::string& default_value = "") const;

    int get_int(const std::string& section, const std::string& key,
               int default_value = 0) const;

private:
    ConfigManager() = default;

    // Parse INI/CONF format file
    bool parse_config_file(const std::string& file_path);

    // Parse a configuration line
    void parse_line(const std::string& line, std::string& current_section);

    // Trim whitespace from string
    static std::string trim(const std::string& str);

    // Internal typed getters / section helpers used by get_rpc_config
    double get_double(const std::string& section, const std::string& key, double default_value = 0.0) const;
    bool   get_boolean(const std::string& section, const std::string& key, bool default_value = false) const;
    bool   has_section(const std::string& section) const;
    bool   has_key(const std::string& section, const std::string& key) const;

    // Configuration storage: section -> (key -> value)
    std::map<std::string, std::map<std::string, std::string>> config_;

    // Flag to track if config was loaded
    bool loaded_ = false;
};
