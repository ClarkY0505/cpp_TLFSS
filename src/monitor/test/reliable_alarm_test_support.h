#ifndef TLSSMON_TEST_RELIABLE_ALARM_TEST_SUPPORT_H
#define TLSSMON_TEST_RELIABLE_ALARM_TEST_SUPPORT_H

#include "alarm_protocol.h"
#include "monitor_data.h"
#include "monitor_wire.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ReliableAlarmTest {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(const std::string &name) {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    _path = fs::temp_directory_path() /
            ("monitor-" + name + "-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(static_cast<long long>(tick)) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    std::error_code error;
    fs::remove_all(_path, error);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(_path, error);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  const fs::path &path() const noexcept { return _path; }

private:
  fs::path _path;
};

class UniqueSocket final {
public:
  explicit UniqueSocket(int fd = -1) noexcept : _fd(fd) {}

  ~UniqueSocket() {
    if (_fd >= 0) {
      (void)::close(_fd);
    }
  }

  UniqueSocket(const UniqueSocket &) = delete;
  UniqueSocket &operator=(const UniqueSocket &) = delete;

  UniqueSocket(UniqueSocket &&other) noexcept : _fd(other.release()) {}

  UniqueSocket &operator=(UniqueSocket &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0) {
        (void)::close(_fd);
      }
      _fd = other.release();
    }
    return *this;
  }

  int get() const noexcept { return _fd; }

  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

private:
  int _fd{-1};
};

class BoundTcpListener final {
public:
  BoundTcpListener() {
    _socket = UniqueSocket(::socket(AF_INET, SOCK_STREAM, 0));
    assert(_socket.get() >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(_socket.get(), reinterpret_cast<const sockaddr *>(&address),
                  static_cast<socklen_t>(sizeof(address))) == 0);
    assert(::listen(_socket.get(), 4) == 0);

    socklen_t size = static_cast<socklen_t>(sizeof(address));
    assert(::getsockname(_socket.get(), reinterpret_cast<sockaddr *>(&address),
                         &size) == 0);
    _port = ntohs(address.sin_port);
    assert(_port != 0U);
  }

  std::uint16_t port() const noexcept { return _port; }

private:
  UniqueSocket _socket;
  std::uint16_t _port{0U};
};

inline int remaining_timeout_ms(
    std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = deadline - now;
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (milliseconds < remaining) {
    milliseconds += 1ms;
  }
  return std::max(1, static_cast<int>(milliseconds.count()));
}

inline bool wait_fd(int fd, short events,
                    std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = events;
    const int result = ::poll(&descriptor, 1, remaining_timeout_ms(deadline));
    if (result > 0) {
      if ((descriptor.revents & events) != 0) {
        return true;
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
      }
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return false;
}

inline bool send_all(int fd, const std::uint8_t *data, std::size_t size,
                     std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t offset = 0U;
  while (offset < size) {
    if (!wait_fd(fd, POLLOUT, deadline)) {
      return false;
    }
    const ssize_t sent =
        ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

inline bool receive_all(int fd, std::uint8_t *data, std::size_t size,
                        std::chrono::steady_clock::time_point deadline) {
  std::size_t offset = 0U;
  while (offset < size) {
    if (!wait_fd(fd, POLLIN, deadline)) {
      return false;
    }
    const ssize_t received = ::recv(fd, data + offset, size - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

inline std::optional<TLSSMON::AlarmWire::AlarmFrame>
receive_frame(int fd, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, TLSSMON::AlarmWire::ALARM_HEADER_SIZE> header{};
  if (!receive_all(fd, header.data(), header.size(), deadline)) {
    return std::nullopt;
  }

  const auto size = TLSSMON::AlarmWire::alarm_frame_size(
      header.data(), header.size());
  if (!size.success()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> wire(*size._frame_size);
  std::copy(header.begin(), header.end(), wire.begin());
  const std::size_t payload_size = wire.size() - header.size();
  if (payload_size != 0U &&
      !receive_all(fd, wire.data() + header.size(), payload_size, deadline)) {
    return std::nullopt;
  }

  auto decoded = TLSSMON::AlarmWire::decode_alarm_frame(
      wire.data(), wire.size());
  return decoded.success() ? std::move(decoded._frame) : std::nullopt;
}

inline UniqueSocket connect_loopback(std::uint16_t port) {
  UniqueSocket socket(::socket(AF_INET, SOCK_STREAM, 0));
  if (socket.get() < 0) {
    return UniqueSocket{};
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                static_cast<socklen_t>(sizeof(address))) != 0) {
    return UniqueSocket{};
  }
  return socket;
}

inline bool wait_for_close(int fd,
                           std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, remaining_timeout_ms(deadline));
    if (result > 0) {
      std::array<std::uint8_t, 64U> buffer{};
      const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (received == 0) {
        return true;
      }
      if (received < 0 && errno != EINTR) {
        return true;
      }
    } else if (result < 0 && errno != EINTR) {
      return true;
    }
  }
  return false;
}

template <typename Predicate>
inline bool wait_until(Predicate predicate,
                       std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

inline TLSSMON::AlarmWire::AlarmMessageId
make_message_id(std::uint8_t seed) {
  TLSSMON::AlarmWire::AlarmMessageId id{};
  for (std::size_t index = 0U; index < id.size(); ++index) {
    id[index] = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}

inline TLSSMON::MonData::StoredRecord
make_record(std::uint32_t event_id, std::uint32_t value,
            std::int64_t seconds = 1'700'000'000,
            std::string description = "network alarm") {
  return TLSSMON::MonData::StoredRecord{
      TLSSMON::MonData::MonitorData{
          TLSSMON::MonData::MonitorKey{0x100U, 2U, 0x200U, event_id},
          std::move(description),
          TLSSMON::MonData::NumericValue{value, 2U}},
      TLSSMON::MonData::MonitorTimestamp{
          std::chrono::seconds{seconds} +
          std::chrono::nanoseconds{123'456'789}}};
}

inline TLSSMON::AlarmWire::AlarmFrame
make_alarm(const TLSSMON::MonData::StoredRecord &record,
           std::uint64_t source_id, std::uint8_t message_seed,
           std::uint64_t envelope_timestamp_ms = 0U) {
  const auto payload = TLSSMON::Wire::encode_v2(record);
  assert(payload._status == TLSSMON::Wire::WireStatus::SUCCESS);

  TLSSMON::AlarmWire::AlarmFrame frame;
  frame._type = TLSSMON::AlarmWire::AlarmFrameType::ALARM;
  frame._source_id = source_id;
  frame._message_id = make_message_id(message_seed);
  frame._timestamp_ms = envelope_timestamp_ms != 0U
                            ? envelope_timestamp_ms
                            : static_cast<std::uint64_t>(
                                  std::chrono::duration_cast<
                                      std::chrono::milliseconds>(
                                      record._changed_at.time_since_epoch())
                                      .count());
  frame._payload = payload._bytes;
  return frame;
}

inline std::vector<std::uint8_t>
encode_frame(const TLSSMON::AlarmWire::AlarmFrame &frame) {
  const auto encoded = TLSSMON::AlarmWire::encode_alarm_frame(frame);
  assert(encoded.success());
  return encoded._bytes;
}

inline bool send_fragmented(int fd, const std::vector<std::uint8_t> &bytes) {
  const std::array<std::size_t, 6U> chunks{1U, 2U, 5U, 13U, 3U, 37U};
  std::size_t offset = 0U;
  std::size_t index = 0U;
  while (offset < bytes.size()) {
    const std::size_t amount =
        std::min(chunks[index % chunks.size()], bytes.size() - offset);
    if (!send_all(fd, bytes.data() + offset, amount)) {
      return false;
    }
    offset += amount;
    ++index;
  }
  return true;
}

} // namespace ReliableAlarmTest

#endif // TLSSMON_TEST_RELIABLE_ALARM_TEST_SUPPORT_H
