#include "common/base/tlss_thread.h"
#include "ftp/server.h"
#include "logger/logger.h"

#include <pthread.h>
#include <csignal>
#include <iostream>
#include <thread>
void logger_init() {
  TLSSLOG::Logger::initBoth("net", "logs/net.log", spdlog::level::info);
  TLSSLOG::Logger::initBoth("storage_service", "logs/storage_service.log", spdlog::level::info);
  TLSSLOG::Logger::initBoth("mysql", "logs/mysql.log", spdlog::level::info);
  TLSSLOG::Logger::get("net")->flush_on(spdlog::level::info);
  TLSSLOG::Logger::get("storage_service")->flush_on(spdlog::level::info);
  TLSSLOG::Logger::get("mysql")->flush_on(spdlog::level::info);
}

// 主线程运行 EventLoop；控制线程接收终止信号并执行需要等待回调的停机流程。
int main() {
  // 在创建任何工作线程前阻塞信号，由控制线程同步等待退出请求。
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
    return 1;
  }
  logger_init();
  bool stopped = false;
  {
    TLSS::NET::EventLoop loop;
    TLSS::NET::InetAddress addr(9000);
    TLSS::FileStorage::StorageService server(&loop, addr);
    server.start();
    TLSS::BASE::Thread main_loop_shutdown(
        [&] {
          int signal_number = 0;
          if (sigwait(&shutdown_signals, &signal_number) == 0) {
            stopped = server.stop();
          }
        },
        "mainLoop_shutdown_thread");
    main_loop_shutdown.start();
    // 信号由专用线程同步接收，避免在异步信号处理函数中操作锁和资源。
    /* std::thread shutdown_thread([&] { */
    /* int signal_number = 0; */
    /* if (sigwait(&shutdown_signals, &signal_number) == 0) { */
    /* stopped = server.stop(); */
    /* } */
    /* }); */
    loop.loop();
    /* shutdown_thread.join(); */
    main_loop_shutdown.join();
  }

  // 所有服务对象和后台线程销毁后，日志系统才能关闭。
  TLSSLOG::Logger::shutdown();
  return stopped ? 0 : 1;
}
