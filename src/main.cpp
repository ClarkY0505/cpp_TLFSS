#include "logger/logger.h"
#include "ftp/server.h"

#include <iostream>
void logger_init(){
    TLSSLOG::Logger::initBoth("net", "logs/net.log", spdlog::level::info);
    TLSSLOG::Logger::initBoth("storage_service", "logs/storage_service.log", spdlog::level::info);
    TLSSLOG::Logger::initBoth("mysql", "logs/mysql.log", spdlog::level::info);
    TLSSLOG::Logger::get("net")->flush_on(spdlog::level::info);
    TLSSLOG::Logger::get("storage_service")->flush_on(spdlog::level::info);
    TLSSLOG::Logger::get("mysql")->flush_on(spdlog::level::info);
}

int main()
{
    logger_init();
    TLSS::NET::EventLoop loop;
    TLSS::NET::InetAddress addr(9000);
    TLSS::FileStorage::StorageService server(&loop,addr);
    server.start();
    loop.loop();

    TLSSLOG:: Logger::shutdown();
    return 0;
}

