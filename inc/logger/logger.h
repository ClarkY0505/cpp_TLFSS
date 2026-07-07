#pragma once

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include <memory>
#include <string>
#include <vector>

class Logger
{
public:
    using LoggerPtr = std::shared_ptr<spdlog::logger>;

    static void initConsole(const std::string &name = "tldss",
                            spdlog::level::level_enum level = spdlog::level::info,
                            const std::string &pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    static void initFile(const std::string &name = "tldss",
                         const std::string &filepath = "logs/tldss.log",
                         spdlog::level::level_enum level = spdlog::level::info,
                         const std::string &pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    static void initRotating(const std::string &name = "tldss",
                             const std::string &filepath = "logs/tldss.log",
                             size_t maxFileSize = 10 * 1024 * 1024,
                             size_t maxFiles = 3,
                             spdlog::level::level_enum level = spdlog::level::info,
                             const std::string &pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    static void initBoth(const std::string &name = "tldss",
                         const std::string &filepath = "logs/tldss.log",
                         spdlog::level::level_enum level = spdlog::level::info,
                         const std::string &pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    static LoggerPtr get(const std::string &name = "tldss");
    static void setLevel(const std::string &name, spdlog::level::level_enum level);
    static void setLevel(spdlog::level::level_enum level);
    static void flush(const std::string &name = "tldss");
    static void shutdown();

};

#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(Logger::get(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(Logger::get(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(Logger::get(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(Logger::get(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(Logger::get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(Logger::get(), __VA_ARGS__)

#define LOG_NAMED(logger, lvl, ...) SPDLOG_LOGGER_CALL(logger, lvl, __VA_ARGS__)
