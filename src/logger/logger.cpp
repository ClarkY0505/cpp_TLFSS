#include "../../inc/logger/logger.h"

void Logger::initConsole(const std::string &name,
                         spdlog::level::level_enum level,
                         const std::string &pattern)
{
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(level);
    logger->set_pattern(pattern);
    spdlog::set_default_logger(logger);
}

void Logger::initFile(const std::string &name,
                      const std::string &filepath,
                      spdlog::level::level_enum level,
                      const std::string &pattern)
{
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filepath);
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(level);
    logger->set_pattern(pattern);
    spdlog::set_default_logger(logger);
}

void Logger::initRotating(const std::string &name,
                          const std::string &filepath,
                          size_t maxFileSize,
                          size_t maxFiles,
                          spdlog::level::level_enum level,
                          const std::string &pattern)
{
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filepath, maxFileSize, maxFiles);
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(level);
    logger->set_pattern(pattern);
    spdlog::set_default_logger(logger);
}

void Logger::initBoth(const std::string &name,
                      const std::string &filepath,
                      spdlog::level::level_enum level,
                      const std::string &pattern)
{
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filepath);
    std::vector<spdlog::sink_ptr> sinks{consoleSink, fileSink};
    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->set_pattern(pattern);
    spdlog::set_default_logger(logger);
}

Logger::LoggerPtr Logger::get(const std::string &name)
{
    auto logger = spdlog::get(name);
    if (!logger)
    {
        logger = spdlog::default_logger();
    }
    return logger;
}

void Logger::setLevel(const std::string &name, spdlog::level::level_enum level)
{
    auto logger = spdlog::get(name);
    if (logger)
    {
        logger->set_level(level);
    }
}

void Logger::setLevel(spdlog::level::level_enum level)
{
    spdlog::set_level(level);
}

void Logger::flush(const std::string &name)
{
    auto logger = spdlog::get(name);
    if (logger)
    {
        logger->flush();
    }
}

void Logger::shutdown()
{
    spdlog::shutdown();
}
