#include <iostream>
#include <vector>
#include <set>
#include "../inc/logger/logger.h"

/* choose one of the folowing two options*/
using namespace std;


/* using std::cout; */
/* using std::endl; */
/* using std::cin; */
/* using std::vector; */
/* using std::set; */

void function_proccess(){
    // 主 logger — 控制台输出
    Logger::initConsole("main", spdlog::level::debug);

    // 为不同模块创建独立的 logger
    Logger::initFile("ftp", "../logs/ftp.log", spdlog::level::info);
    Logger::initFile("gateway", "../logs/gateway.log", spdlog::level::warn);
    Logger::initBoth("monitor", "../logs/monitor.log", spdlog::level::debug);

    // --- 使用不同 logger ---
    auto ftpLog    = Logger::get("ftp");
    auto gwLog     = Logger::get("gateway");
    auto monitorLog = Logger::get("monitor");

    ftpLog->info("FTP server started on port 21");
    gwLog->warn("No healthy backends available");
    monitorLog->debug("Heartbeat sent");

    // 或者用默认 logger (名称为 "main")
    LOG_INFO("App initialised");

    // 用宏向指定 logger 输出
    LOG_NAMED(gwLog, spdlog::level::err, "Gateway crash: {}", "timeout");

    // 运行时改级别
    Logger::setLevel("ftp", spdlog::level::trace);

    Logger::shutdown();

}

int main()
{
    function_proccess();
    cout << "hello world!!!!!" << endl;
    return 0;
}

