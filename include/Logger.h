#pragma once

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include <spdlog/spdlog.h>
#include <memory>

class Logger {
public:
    static void Init();
    static std::shared_ptr<spdlog::logger> Get();

private:
    static std::shared_ptr<spdlog::logger> logger_;
};

#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(Logger::Get(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(Logger::Get(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(Logger::Get(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(Logger::Get(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(Logger::Get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(Logger::Get(), __VA_ARGS__)
