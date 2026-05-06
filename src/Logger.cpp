#include "Logger.h"
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

std::shared_ptr<spdlog::logger> Logger::logger_;

void Logger::Init() {
    spdlog::init_thread_pool(8192, 1);
    
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/asycdisk.log", 1048576 * 5, 3);
    
    std::vector<spdlog::sink_ptr> sinks {stdout_sink, file_sink};
    
    logger_ = std::make_shared<spdlog::async_logger>("AsyCDisk", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] [%s:%#] %v");
    logger_->set_level(spdlog::level::info);
    logger_->flush_on(spdlog::level::info);
    
    spdlog::register_logger(logger_);
    spdlog::set_default_logger(logger_);
}

std::shared_ptr<spdlog::logger> Logger::Get() {
    return logger_;
}
