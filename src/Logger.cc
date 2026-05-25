#include "Logger.h"
#include <iomanip>
#include <iostream>

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void Logger::init(const std::string& log_path, LogLevel level) {
    std::scoped_lock<std::recursive_mutex> lock(mutex_);

    current_level_ = level;

    log_file_.open(log_path, std::ios::app);
    if (!log_file_.is_open()) {
        std::cerr << "Warning: Failed to open log file: " << log_path << std::endl;
        return;
    }

    initialized_ = true;
    info("Logger initialized");
}

void Logger::set_log_level(LogLevel level) {
    std::scoped_lock<std::recursive_mutex> lock(mutex_);
    current_level_ = level;
}

void Logger::debug(const std::string& message) {
    if (current_level_ <= LogLevel::DEBUG) {
        log(LogLevel::DEBUG, message);
    }
}

void Logger::info(const std::string& message) {
    if (current_level_ <= LogLevel::INFO) {
        log(LogLevel::INFO, message);
    }
}

void Logger::warn(const std::string& message) {
    if (current_level_ <= LogLevel::WARN) {
        log(LogLevel::WARN, message);
    }
}

void Logger::error(const std::string& message) {
    if (current_level_ <= LogLevel::ERROR) {
        log(LogLevel::ERROR, message);
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    std::scoped_lock<std::recursive_mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << message << std::endl;
        return;
    }

    std::string formatted_log = format_log(level, message);

    // Write to file (plain text, no colors)
    if (log_file_.is_open()) {
        log_file_ << formatted_log << std::endl;
        log_file_.flush();
    }

    // Output to console with colors
    std::string colored_log = get_colored_console_output(level, formatted_log);
    std::cout << colored_log << std::endl;
}

std::string Logger::format_log(LogLevel level, const std::string& message) {
    // Get current timestamp
    auto now = std::chrono::high_resolution_clock::now();
    auto time = std::chrono::high_resolution_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    ss << " [" << get_level_string(level) << "] ";
    ss << message;

    return ss.str();
}

std::string Logger::get_level_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string Logger::get_colored_console_output(LogLevel level, const std::string& formatted_log) {
    // ANSI color codes
    const std::string GREEN = "\033[32m";
    const std::string LIGHT_GREEN = "\033[92m";
    const std::string BLUE = "\033[94m";
    const std::string YELLOW = "\033[93m";
    const std::string RED = "\033[91m";
    const std::string RESET = "\033[0m";

    std::string color;
    switch (level) {
        case LogLevel::DEBUG:
            color = LIGHT_GREEN;
            break;
        case LogLevel::INFO:
            color = BLUE;
            break;
        case LogLevel::WARN:
            color = YELLOW;
            break;
        case LogLevel::ERROR:
            color = RED;
            break;
        default:
            color = RESET;
    }

    return color + formatted_log + RESET;
}
