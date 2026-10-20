#include "common/net/event_loop.h"
#include "ftp/client.h"
#include "logger/logger.h"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: tlss-cli <IPv4> <port>\n";
    return 1;
  }
  const auto parsed_port = std::stoul(argv[2]);
  if (parsed_port == 0 || parsed_port > 65535) {
    std::cerr << "Invalid port\n";
    return 1;
  }
  TLSSLOG::Logger::initConsole("net", spdlog::level::warn);
  {
    TLSS::NET::EventLoop loop;
    TLSS::NET::InetAddress address(static_cast<std::uint16_t>(parsed_port), argv[1]);
    TLSS::FileStorage::Client client(&loop, address);
    client.start();
    loop.loop();
  } 
  TLSSLOG::Logger::shutdown();
  return 0;
}
