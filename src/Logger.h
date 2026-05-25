#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <format>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    // Get singleton instance
    static Logger& instance();

    // Initialize logger with file path and log level
    void init(const std::string& log_path, LogLevel level = LogLevel::INFO);

    // Set log level
    void set_log_level(LogLevel level);

    // Log messages with different levels
    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    // Variadic template versions for formatted logging
    template<typename... Args>
    void debugf(std::string_view format, Args... args) {
        if (current_level_ <= LogLevel::DEBUG) {
            log(LogLevel::DEBUG, std::vformat(format, std::make_format_args(args...)));
        }
    }

    template<typename... Args>
    void infof(std::string_view format, Args... args) {
        if (current_level_ <= LogLevel::INFO) {
            log(LogLevel::INFO, std::vformat(format, std::make_format_args(args...)));
        }
    }

    template<typename... Args>
    void warnf(std::string_view format, Args... args) {
        if (current_level_ <= LogLevel::WARN) {
            log(LogLevel::WARN, std::vformat(format, std::make_format_args(args...)));
        }
    }

    template<typename... Args>
    void errorf(std::string_view format, Args... args) {
        if (current_level_ <= LogLevel::ERROR) {
            log(LogLevel::ERROR, std::vformat(format, std::make_format_args(args...)));
        }
    }

private:
    Logger() = default;
    ~Logger();

    // Write log entry
    void log(LogLevel level, const std::string& message);

    // Format log entry with timestamp
    std::string format_log(LogLevel level, const std::string& message);

    // Get log level string
    static std::string get_level_string(LogLevel level);

    // Get colored console output with ANSI color codes
    static std::string get_colored_console_output(LogLevel level, const std::string& formatted_log);

    // File stream for logging
    std::ofstream log_file_;
    LogLevel current_level_ = LogLevel::INFO;
    bool initialized_ = false;
    std::recursive_mutex mutex_;  // Changed to recursive_mutex to allow nested locking
};
