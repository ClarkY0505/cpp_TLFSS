#include "common/base/tlss_thread.h"
#include "ftp/server.h"
#include "ftp/storage_root_lock.h"
#include "logger/logger.h"
#include "observability/storage_monitor_runtime.h"

#include <pthread.h>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void logger_init() {
  TLSSLOG::Logger::initBoth("net", "logs/net.log", spdlog::level::info);
  TLSSLOG::Logger::initBoth("storage_service", "logs/storage_service.log", spdlog::level::info);
  TLSSLOG::Logger::initBoth("mysql", "logs/mysql.log", spdlog::level::info);
  TLSSLOG::Logger::get("net")->flush_on(spdlog::level::info);
  TLSSLOG::Logger::get("storage_service")->flush_on(spdlog::level::info);
  TLSSLOG::Logger::get("mysql")->flush_on(spdlog::level::info);
}
std::chrono::milliseconds resource_interval() {
  const char* setting = std::getenv("TLSS_RESOURCE_INTERVAL_MS");
  if (!setting)
    return std::chrono::milliseconds{5000};
  const std::string text(setting);
  std::uint64_t milliseconds = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || milliseconds == 0 ||
      milliseconds > 3600000)
    throw std::invalid_argument("invalid TLSS_RESOURCE_INTERVAL_MS");
  return std::chrono::milliseconds{milliseconds};
}
}  // namespace

int main() {
  // All subsequently created threads inherit this mask. Only the control thread waits for signals.
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0)
    return 1;
  logger_init();
  bool stopped = false;
  try {
    using namespace TLSS::OBSERVABILITY;
    auto metrics = std::make_shared<StorageMetrics>();
    auto cache = std::make_shared<TLSS::FileStorage::MetadataCache>();
    StorageMonitorConfig monitor_config;
    bool monitoring = true;
    auto interval = std::chrono::milliseconds{5000};
    try {
      if (const char* enabled = std::getenv("TLSS_MONITOR_ENABLED")) {
        const std::string value(enabled);
        if (value != "0" && value != "1")
          throw std::invalid_argument("invalid TLSS_MONITOR_ENABLED");
        monitoring = value == "1";
      }
      if (monitoring) {
        monitor_config = monitor_config_from_environment();
        interval = resource_interval();
        monitor_config.resource_stale_after =
            std::max(std::chrono::milliseconds{15000}, interval * 3);
      }
    } catch (const std::exception& error) {
      monitoring = false;
      TLSSLOG::Logger::get("storage_service")->error("Monitoring disabled: {}", error.what());
    }
    const char* configured_root = std::getenv("TLSS_STORAGE_ROOT");
    const auto storage_root = std::filesystem::absolute(
        configured_root && *configured_root ? std::filesystem::path(configured_root)
                                            : std::filesystem::path("./data/objects"));
    TLSS::FileStorage::StorageRootLock root_lock(storage_root);
    auto sampler = std::make_shared<SystemResourceSampler>(
        SystemResourceSamplerConfig{storage_root, interval, "/proc"});
    TLSS::NET::EventLoop loop;
    TLSS::NET::InetAddress address(9000);
    TLSS::FileStorage::StorageService server(&loop, address, "tlss_service", 4, 8, 256, metrics,
                                             cache);
    bool storage_ready = false;
    try {
      storage_ready = server.prepare_storage();
      if (!storage_ready) {
        TLSSLOG::Logger::get("storage_service")->error("prepare_storage failed");
      }
    } catch (const std::exception& error) {
      TLSSLOG::Logger::get("storage_service")
          ->error("server storage preapare disabled: {}", error.what());
    }

    if (storage_ready) {
      StorageMonitorRuntime monitor(monitor_config, metrics, sampler, [&server] {
        server.sample_metrics();
      });
      if (monitoring) {
        if (!sampler->start()) {
          TLSSLOG::Logger::get("storage_service")->error("Resource sampler failed to start");
        }
        if (!monitor.start()) {
          TLSSLOG::Logger::get("storage_service")
              ->error("Monitoring disabled: {}", monitor.status().error);
          sampler->finish_and_stop();
        }
      }
      server.start();
      TLSS::BASE::Thread control(
          [&] {
            int signal_number = 0;
            if (sigwait(&shutdown_signals, &signal_number) == 0) {
              stopped = server.stop();
              sampler->finish_and_stop();
              monitor.finish_and_stop();
            }
          },
          "mainLoop_shutdown_thread");
      control.start();
      loop.loop();
      control.join();
    }
  } catch (const std::exception& error) {
    TLSSLOG::Logger::get("storage_service")->error("Storage service failed: {}", error.what());
  }
  TLSSLOG::Logger::shutdown();
  return stopped ? 0 : 1;
}
