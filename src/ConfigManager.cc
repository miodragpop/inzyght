#include "ConfigManager.h"
#include "Logger.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <format>

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load_config(const std::string& config_path) {
    Logger& logger = Logger::instance();
    logger.debugf("Loading configuration from: {}", config_path);

    if (parse_config_file(config_path)) {
        loaded_ = true;
        logger.info("Configuration loaded successfully");
        return true;
    } else {
        logger.errorf("Failed to load configuration from: {}", config_path);
        return false;
    }
}

bool ConfigManager::parse_config_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open configuration file: " << file_path << std::endl;
        return false;
    }

    std::string line;
    std::string current_section;
    //int line_number = 0;

    while (std::getline(file, line)) {
        //line_number++;

        // Trim whitespace
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Parse the line
        parse_line(line, current_section);
    }

    file.close();
    return true;
}

void ConfigManager::parse_line(const std::string& line, std::string& current_section) {
    // Check for section header [section]
    if (line[0] == '[' && line[line.length() - 1] == ']') {
        current_section = line.substr(1, line.length() - 2);
        current_section = trim(current_section);
        return;
    }

    // Parse key = value
    size_t equal_pos = line.find('=');
    if (equal_pos != std::string::npos && !current_section.empty()) {
        std::string key = line.substr(0, equal_pos);
        std::string value = line.substr(equal_pos + 1);

        key = trim(key);
        value = trim(value);

        // Remove quotes if present
        if (!value.empty() && value[0] == '"' && value[value.length() - 1] == '"') {
            value = value.substr(1, value.length() - 2);
        }

        config_[current_section][key] = value;
    }
}

std::string ConfigManager::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";

    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, (end - start + 1));
}

RpcConfig::Config ConfigManager::get_rpc_config() const {
    RpcConfig::Config config = RpcConfig::get_default();

    if (has_section("rpc")) {
        config.host = get_string("rpc", "host", config.host);
        config.port = get_int("rpc", "port", config.port);
        config.username = get_string("rpc", "username", config.username);
        config.password = get_string("rpc", "password", config.password);
        config.timeout = get_double("rpc", "timeout", config.timeout);
        config.ssl = get_boolean("rpc", "ssl", config.ssl);
        config.validate_certificate = get_boolean("rpc", "validate_certificate", config.validate_certificate);
    }

    if (has_section("connection")) {
        config.pool_size = get_int("connection", "pool_size", config.pool_size);
        config.connection_timeout = get_double("connection", "connection_timeout", config.connection_timeout);
    }

    if (has_section("zeromq")) {
        config.zmq_enabled = get_boolean("zeromq", "enabled", true);
        // New format: single endpoint
        if (has_key("zeromq", "endpoint")) {
            config.zmq_endpoint = get_string("zeromq", "endpoint", "tcp://127.0.0.1:48332");
        }
        // Backward compatibility: if old format exists, use hashblock_endpoint
        else if (has_key("zeromq", "hashblock_endpoint")) {
            config.zmq_endpoint = get_string("zeromq", "hashblock_endpoint", "tcp://127.0.0.1:48332");
            std::cerr << "Warning: Using deprecated 'hashblock_endpoint' config. Please update to 'endpoint'." << std::endl;
        }
        config.zmq_polling_fallback_timeout = get_int("zeromq", "polling_fallback_timeout", 300);
        config.zmq_polling_interval = get_int("zeromq", "polling_interval", 300);  // 5 minutes default
    }

    if (has_section("postgresql")) {
        config.postgresql_host = get_string("postgresql", "host", "127.0.0.1");
        config.postgresql_port = get_int("postgresql", "port", 5432);
        config.postgresql_database = get_string("postgresql", "database", "inzyght");
        // No insecure defaults for credentials — empty string lets libpq
        // surface a clear authentication error rather than silently
        // attempting login with well-known weak defaults.
        config.postgresql_username = get_string("postgresql", "username", "");
        config.postgresql_password = get_string("postgresql", "password", "");
    }

    if (has_section("indexer")) {
        config.block_batch_size = get_int("indexer", "block_batch_size", 25);
        config.block_headers_batch_size = get_int("indexer", "block_headers_batch_size", 100);
        config.queue_depth = get_int("indexer", "queue_depth", 5);
    }

    return config;
}

std::string ConfigManager::get_server_host() const {
    return get_string("server", "host", "127.0.0.1");
}

int ConfigManager::get_server_port() const {
    return get_int("server", "port", 8080);
}

int ConfigManager::get_thread_count() const {
    return get_int("server", "threads", 4);
}

std::string ConfigManager::get_log_level() const {
    return get_string("server", "log_level", "INFO");
}

std::string ConfigManager::get_database_type() const {
    return get_string("database", "type", "sqlite");
}

std::string ConfigManager::get_string(const std::string& section, const std::string& key,
                                     const std::string& default_value) const {
    auto section_it = config_.find(section);
    if (section_it != config_.end()) {
        auto key_it = section_it->second.find(key);
        if (key_it != section_it->second.end()) {
            return key_it->second;
        }
    }
    return default_value;
}

int ConfigManager::get_int(const std::string& section, const std::string& key,
                          int default_value) const {
    std::string value = get_string(section, key);
    if (value.empty()) {
        return default_value;
    }

    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        std::cerr << std::format("Warning: Invalid integer value for {}.{} = {}: {}",
                                section, key, value, e.what()) << std::endl;
        return default_value;
    }
}

double ConfigManager::get_double(const std::string& section, const std::string& key, double default_value) const {
    std::string value = get_string(section, key);
    if (value.empty()) return default_value;
    try {
        return std::stod(value);
    } catch (const std::exception& e) {
        std::cerr << std::format("Warning: Invalid double value for {}.{} = {}: {}",
                                section, key, value, e.what()) << std::endl;
        return default_value;
    }
}

bool ConfigManager::get_boolean(const std::string& section, const std::string& key, bool default_value) const {
    std::string value = get_string(section, key);
    if (value.empty()) return default_value;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (value == "true" || value == "yes" || value == "1" || value == "on")  return true;
    if (value == "false" || value == "no" || value == "0" || value == "off") return false;
    std::cerr << "Warning: Invalid boolean value for " << section << "." << key
              << " = " << value << std::endl;
    return default_value;
}

bool ConfigManager::has_section(const std::string& section) const {
    return config_.find(section) != config_.end();
}

bool ConfigManager::has_key(const std::string& section, const std::string& key) const {
    auto section_it = config_.find(section);
    if (section_it == config_.end()) return false;
    return section_it->second.find(key) != section_it->second.end();
}
