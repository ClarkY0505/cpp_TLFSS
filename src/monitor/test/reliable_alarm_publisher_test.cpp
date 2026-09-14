#include "alarm_protocol.h"
#include "alarm_spool.h"
#include "engine.h"
#include "monitor_wire.h"
#include "reliable_alarm_publisher.h"

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
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t SOURCE_ID = UINT64_C(0x1020304050607080);

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

class LoopbackTcpListener final {
public:
  explicit LoopbackTcpListener(std::uint16_t requested_port = 0U) {
    _socket = UniqueSocket(::socket(AF_INET, SOCK_STREAM, 0));
    assert(_socket.get() >= 0);

    const int enabled = 1;
    assert(::setsockopt(_socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                        static_cast<socklen_t>(sizeof(enabled))) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(requested_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    assert(::bind(_socket.get(), reinterpret_cast<const sockaddr *>(&address),
                  static_cast<socklen_t>(sizeof(address))) == 0);
    assert(::listen(_socket.get(), 16) == 0);

    socklen_t size = static_cast<socklen_t>(sizeof(address));
    assert(::getsockname(_socket.get(), reinterpret_cast<sockaddr *>(&address),
                         &size) == 0);
    _port = ntohs(address.sin_port);
    assert(_port != 0U);
  }

  LoopbackTcpListener(const LoopbackTcpListener &) = delete;
  LoopbackTcpListener &operator=(const LoopbackTcpListener &) = delete;

  std::uint16_t port() const noexcept { return _port; }

  UniqueSocket accept_for(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());

      pollfd descriptor{};
      descriptor.fd = _socket.get();
      descriptor.events = POLLIN;

      const int result =
          ::poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));

      if (result > 0 && (descriptor.revents & POLLIN) != 0) {
        return UniqueSocket(::accept(_socket.get(), nullptr, nullptr));
      }

      if (result < 0 && errno == EINTR) {
        continue;
      }

      if (result <= 0) {
        break;
      }
    }

    return UniqueSocket{};
  }

private:
  UniqueSocket _socket;
  std::uint16_t _port{0U};
};

int remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
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

bool wait_fd(int fd, short events,
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

bool send_all(int fd, const std::uint8_t *data, std::size_t size,
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

bool receive_all(int fd, std::uint8_t *data, std::size_t size,
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

std::optional<AlarmWire::AlarmFrame>
receive_frame(int fd, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, AlarmWire::ALARM_HEADER_SIZE> header{};

  if (!receive_all(fd, header.data(), header.size(), deadline)) {
    return std::nullopt;
  }

  const AlarmWire::AlarmFrameSizeResult size =
      AlarmWire::alarm_frame_size(header.data(), header.size());
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

  AlarmWire::AlarmDecodeResult decoded =
      AlarmWire::decode_alarm_frame(wire.data(), wire.size());
  if (!decoded.success()) {
    return std::nullopt;
  }

  return std::move(decoded._frame);
}

bool send_control_frame(int fd, AlarmWire::AlarmFrameType type,
                        const AlarmWire::AlarmFrame &request) {
  AlarmWire::AlarmFrame response;
  response._type = type;
  response._source_id = request._source_id;
  response._message_id = request._message_id;
  response._timestamp_ms = request._timestamp_ms;

  const AlarmWire::AlarmEncodeResult encoded =
      AlarmWire::encode_alarm_frame(response);
  return encoded.success() &&
         send_all(fd, encoded._bytes.data(), encoded._bytes.size());
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

std::size_t regular_file_count(const fs::path &directory) {
  std::error_code error;

  if (!fs::is_directory(directory, error) || error) {
    return 0U;
  }

  std::size_t count = 0U;
  fs::directory_iterator iterator(directory, error);
  const fs::directory_iterator end;

  while (!error && iterator != end) {
    if (iterator->is_regular_file(error) && !error) {
      ++count;
    }
    iterator.increment(error);
  }

  return error ? 0U : count;
}

std::size_t pending_file_count(const fs::path &root) {
  return regular_file_count(root / "pending");
}

std::uint16_t reserve_unused_loopback_port() {
  UniqueSocket socket(::socket(AF_INET, SOCK_STREAM, 0));
  assert(socket.get() >= 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(0U);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                static_cast<socklen_t>(sizeof(address))) == 0);

  socklen_t size = static_cast<socklen_t>(sizeof(address));
  assert(::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&address),
                       &size) == 0);
  return ntohs(address.sin_port);
}

ReliableAlarmPublisherConfig make_config(const fs::path &root,
                                         std::uint16_t port) {
  return ReliableAlarmPublisherConfig{
      "127.0.0.1", port, SOURCE_ID, root, 0U, 1s, 250ms};
}

MonData::StoredRecord make_record(std::uint32_t sequence = 1U) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{0x100U, 2U, 0x200U, sequence},
          "network connection failure " + std::to_string(sequence),
          MonData::NumericValue{sequence, 2U}},
      MonData::MonitorTimestamp{
          std::chrono::seconds{1'700'000'000 + sequence} +
          std::chrono::nanoseconds{123'456'789}}};
}

const MonData::NumericValue &numeric_value(const MonData::MonitorData &data) {
  assert(std::holds_alternative<MonData::NumericValue>(data._value));
  return std::get<MonData::NumericValue>(data._value);
}

/*
 * 配置错误必须在启动后台线程之前被拒绝，并返回稳定的 INVALID_CONFIG。
 * outbox 根路径是普通文件时，则应明确报告 SPOOL_ERROR。
 */
void test_configuration_and_setup_status() {
  TemporaryDirectory root("reliable-config");
  const std::uint16_t port = reserve_unused_loopback_port();
  const ReliableAlarmPublisherConfig valid = make_config(root.path(), port);

  const auto assert_invalid = [](ReliableAlarmPublisherConfig config) {
    ReliableAlarmPublisher publisher(std::move(config));
    assert(!publisher.ready());
    assert(publisher.setup_status() ==
           ReliableAlarmPublisherSetupStatus::INVALID_CONFIG);
    assert(publisher.setup_error() == EINVAL);
  };

  auto invalid = valid;
  invalid._collector_host.clear();
  assert_invalid(invalid);

  invalid = valid;
  invalid._collector_port = 0U;
  assert_invalid(invalid);

  invalid = valid;
  invalid._source_id = 0U;
  assert_invalid(invalid);

  invalid = valid;
  invalid._outbox_dir.clear();
  assert_invalid(invalid);

  invalid = valid;
  invalid._ack_timeout = 0ms;
  assert_invalid(invalid);

  invalid = valid;
  invalid._max_backoff = 99ms;
  assert_invalid(invalid);

  TemporaryDirectory file_root("reliable-spool-error");
  {
    std::ofstream output(file_root.path());
    output << "not a directory";
  }
  ReliableAlarmPublisher spool_error(make_config(file_root.path(), port));
  assert(!spool_error.ready());
  assert(spool_error.setup_status() ==
         ReliableAlarmPublisherSetupStatus::SPOOL_ERROR);
  assert(spool_error.setup_error() != 0);
}

/*
 * Collector 不在线时 enqueue() 仍必须成功：它只执行 V2 编码、ALARM
 * 封装和 outbox 原子落盘。析构后读取磁盘，核对完整 Key、值、描述、时间戳
 * 和 source ID，证明业务线程没有依赖网络成功。
 */
void test_enqueue_persists_complete_v2_alarm_without_collector() {
  TemporaryDirectory root("reliable-persist");
  const std::uint16_t port = reserve_unused_loopback_port();
  const MonData::StoredRecord original = make_record(7U);

  {
    ReliableAlarmPublisher publisher(make_config(root.path(), port));
    assert(publisher.ready());
    assert(publisher.setup_status() ==
           ReliableAlarmPublisherSetupStatus::SUCCESS);
    assert(publisher.config()._source_id == SOURCE_ID);

    const AlarmEnqueueResult result = publisher.enqueue(original);
    assert(result._status == AlarmEnqueueStatus::SUCCESS);
    assert(result.durable());
    assert(result._system_error == 0);
    assert(pending_file_count(root.path()) == 1U);
  }

  AlarmWire::AlarmSpool spool(
      {root.path(), AlarmWire::AlarmSpoolKind::OUTBOX, 0U});
  const AlarmWire::AlarmSpoolPathResult next = spool.next();
  assert(next.success());
  const AlarmWire::AlarmSpoolReadResult stored = spool.read(next._path);
  assert(stored.success());

  const AlarmWire::AlarmDecodeResult alarm = AlarmWire::decode_alarm_frame(
      stored._bytes.data(), stored._bytes.size());
  assert(alarm.success());
  assert(alarm._frame->_type == AlarmWire::AlarmFrameType::ALARM);
  assert(alarm._frame->_source_id == SOURCE_ID);
  assert(alarm._frame->_timestamp_ms == UINT64_C(1700000007123));

  const Wire::DecodeResult payload = Wire::decode_v2(
      alarm._frame->_payload.data(), alarm._frame->_payload.size());
  assert(payload._status == Wire::WireStatus::SUCCESS);
  assert(payload._record.has_value());
  assert(payload._record->_version == Wire::WireVersion::V2);
  assert(payload._record->_data._key == original._data._key);
  assert(payload._record->_data._description == original._data._description);
  assert(numeric_value(payload._record->_data) ==
         numeric_value(original._data));
  assert(payload._record->_changed_at == original._changed_at);
}

/*
 * 负时间戳和超过 V2 1200 字节上限的记录不能生成告警文件；容量不足则返回
 * SPOOL_FULL。三个失败都不能把半成品遗留在 pending/。
 */
void test_enqueue_rejects_encoding_errors_and_full_outbox() {
  TemporaryDirectory encode_root("reliable-encode-errors");
  const std::uint16_t port = reserve_unused_loopback_port();

  ReliableAlarmPublisher publisher(make_config(encode_root.path(), port));
  assert(publisher.ready());

  MonData::StoredRecord negative = make_record(1U);
  negative._changed_at = MonData::MonitorTimestamp{-1s};
  assert(publisher.enqueue(std::move(negative))._status ==
         AlarmEnqueueStatus::ENCODE_FAILED);

  MonData::StoredRecord oversized = make_record(2U);
  oversized._data._description.assign(Wire::V2_MAX_DATAGRAM_SIZE, 'x');
  assert(publisher.enqueue(std::move(oversized))._status ==
         AlarmEnqueueStatus::ENCODE_FAILED);
  assert(pending_file_count(encode_root.path()) == 0U);

  TemporaryDirectory full_root("reliable-full");
  ReliableAlarmPublisherConfig full_config = make_config(full_root.path(), port);
  full_config._max_outbox_bytes = 1U;
  ReliableAlarmPublisher full(std::move(full_config));
  assert(full.ready());
  assert(full.enqueue(make_record(3U))._status ==
         AlarmEnqueueStatus::SPOOL_FULL);
  assert(pending_file_count(full_root.path()) == 0U);
}

/*
 * 匹配 source ID 和 message ID 的 ACK 是唯一删除条件。服务端同时解码 V2
 * payload，验证网络发送的内容与 enqueue() 落盘内容一致。
 */
void test_matching_ack_removes_pending_alarm() {
  TemporaryDirectory root("reliable-ack");
  LoopbackTcpListener collector;

  auto received = std::async(std::launch::async, [&collector] {
    UniqueSocket client = collector.accept_for(3s);
    if (client.get() < 0) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }

    auto frame = receive_frame(client.get());
    if (!frame.has_value() ||
        !send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            *frame)) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }
    return frame;
  });

  ReliableAlarmPublisher publisher(make_config(root.path(), collector.port()));
  assert(publisher.ready());
  assert(publisher.enqueue(make_record(10U)).durable());

  const auto alarm = received.get();
  assert(alarm.has_value());
  assert(alarm->_type == AlarmWire::AlarmFrameType::ALARM);
  assert(alarm->_source_id == SOURCE_ID);

  const Wire::DecodeResult payload =
      Wire::decode_v2(alarm->_payload.data(), alarm->_payload.size());
  assert(payload._status == Wire::WireStatus::SUCCESS);
  assert(payload._record->_data._key._eid == 10U);
  assert(wait_until([&] { return pending_file_count(root.path()) == 0U; }));
}

/*
 * 使用阶段 11 暴露的 Engine seam 接入真实 ReliableAlarmPublisher。业务层只
 * 调用 report_error()；可靠回调按值捕获 shared_ptr，确保同步 enqueue 和
 * 后台 ACK 处理期间 Publisher 都存活。
 */
void test_engine_report_error_reaches_reliable_publisher() {
  TemporaryDirectory root("reliable-engine-seam");
  LoopbackTcpListener collector;

  auto server = std::async(std::launch::async, [&collector] {
    UniqueSocket client = collector.accept_for(3s);
    auto alarm = client.get() >= 0 ? receive_frame(client.get()) : std::nullopt;
    if (!alarm.has_value() ||
        !send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            *alarm)) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }
    return alarm;
  });

  Engine engine{MonConfig{"m9-reliable-engine", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  auto publisher = std::make_shared<ReliableAlarmPublisher>(
      make_config(root.path(), collector.port()));
  assert(publisher->ready());
  assert(engine.set_alarm_publisher(
      [publisher](MonData::StoredRecord record) {
        return publisher->enqueue(std::move(record));
      }));

  const MonData::MonitorKey key{0x100U, 2U, 0x200U, 61U};
  const MonData::UpdateResult updated =
      engine.report_error(key, 9001U, "collector connection failed");
  assert(updated._status == MonData::UpdateStatus::INSERTED);
  assert(updated.changed());
  assert(engine.find_data(key).has_value());

  const auto alarm = server.get();
  assert(alarm.has_value());
  const auto payload =
      Wire::decode_v2(alarm->_payload.data(), alarm->_payload.size());
  assert(payload._status == Wire::WireStatus::SUCCESS);
  assert(payload._record->_data._key == key);
  assert(numeric_value(payload._record->_data)._value == 9001U);
  assert(numeric_value(payload._record->_data)._state == 2U);
  assert(wait_until([&] { return pending_file_count(root.path()) == 0U; }));
}

/*
 * ACK 的 source ID 或 message ID 不匹配时必须视为失败，关闭连接并保留
 * pending 文件，不能确认另一条告警。
 */
void test_mismatched_ack_keeps_pending_alarm() {
  TemporaryDirectory root("reliable-wrong-ack");
  LoopbackTcpListener collector;

  auto server = std::async(std::launch::async, [&collector] {
    UniqueSocket client = collector.accept_for(3s);
    const auto alarm =
        client.get() >= 0 ? receive_frame(client.get()) : std::nullopt;
    if (!alarm.has_value()) {
      return false;
    }

    AlarmWire::AlarmFrame wrong;
    wrong._type = AlarmWire::AlarmFrameType::ACK;
    wrong._source_id = alarm->_source_id + 1U;
    wrong._message_id = alarm->_message_id;
    wrong._timestamp_ms = alarm->_timestamp_ms;
    wrong._message_id[0] ^= 0xffU;

    const auto encoded = AlarmWire::encode_alarm_frame(wrong);
    return encoded.success() &&
           send_all(client.get(), encoded._bytes.data(), encoded._bytes.size());
  });

  {
    ReliableAlarmPublisher publisher(
        make_config(root.path(), collector.port()));
    assert(publisher.enqueue(make_record(11U)).durable());
    assert(server.get());
  }

  assert(pending_file_count(root.path()) == 1U);
}

/*
 * 后台状态机同一时刻最多发送一个未确认 ALARM：在第一个 ACK 到来以前，
 * 第二个完整帧不能出现在连接中；确认后才允许发送下一条。
 */
void test_only_one_alarm_is_in_flight() {
  TemporaryDirectory root("reliable-single-flight");
  LoopbackTcpListener collector;

  auto server = std::async(std::launch::async, [&collector] {
    std::vector<AlarmWire::AlarmFrame> alarms;
    UniqueSocket client = collector.accept_for(3s);
    if (client.get() < 0) {
      return alarms;
    }

    auto first = receive_frame(client.get());
    if (!first.has_value()) {
      return alarms;
    }
    alarms.push_back(*first);

    pollfd descriptor{};
    descriptor.fd = client.get();
    descriptor.events = POLLIN;
    const int early_data = ::poll(&descriptor, 1, 150);
    if (early_data != 0) {
      return std::vector<AlarmWire::AlarmFrame>{};
    }

    if (!send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            alarms.front())) {
      return std::vector<AlarmWire::AlarmFrame>{};
    }

    auto second = receive_frame(client.get());
    if (!second.has_value()) {
      return std::vector<AlarmWire::AlarmFrame>{};
    }
    alarms.push_back(*second);

    if (!send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            alarms.back())) {
      return std::vector<AlarmWire::AlarmFrame>{};
    }
    return alarms;
  });

  ReliableAlarmPublisher publisher(make_config(root.path(), collector.port()));
  assert(publisher.enqueue(make_record(20U)).durable());
  assert(publisher.enqueue(make_record(21U)).durable());

  const auto alarms = server.get();
  assert(alarms.size() == 2U);
  assert(alarms[0]._type == AlarmWire::AlarmFrameType::ALARM);
  assert(alarms[1]._type == AlarmWire::AlarmFrameType::ALARM);
  assert(alarms[0]._message_id != alarms[1]._message_id);
  assert(wait_until([&] { return pending_file_count(root.path()) == 0U; }));
}

/*
 * 连续断线必须重发同一 message ID，并采用 100ms、200ms 指数退避；最终
 * 收到匹配 ACK 后删除文件。时间断言只设置下界，避免慢速 CI 的调度抖动。
 */
void test_disconnect_retries_with_exponential_backoff() {
  TemporaryDirectory root("reliable-backoff");
  LoopbackTcpListener collector;

  auto server = std::async(std::launch::async, [&collector] {
    std::vector<AlarmWire::AlarmFrame> alarms;
    std::vector<std::chrono::steady_clock::time_point> arrivals;

    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
      UniqueSocket client = collector.accept_for(4s);
      if (client.get() < 0) {
        break;
      }
      auto alarm = receive_frame(client.get());
      if (!alarm.has_value()) {
        break;
      }
      arrivals.push_back(std::chrono::steady_clock::now());
      alarms.push_back(*alarm);

      if (attempt == 2U &&
          !send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                              *alarm)) {
        alarms.clear();
        break;
      }
    }

    return std::make_pair(std::move(alarms), std::move(arrivals));
  });

  ReliableAlarmPublisherConfig config = make_config(root.path(), collector.port());
  config._max_backoff = 200ms;
  ReliableAlarmPublisher publisher(std::move(config));
  assert(publisher.enqueue(make_record(30U)).durable());

  const auto attempts = server.get();
  assert(attempts.first.size() == 3U);
  assert(attempts.second.size() == 3U);
  assert(attempts.first[0]._message_id == attempts.first[1]._message_id);
  assert(attempts.first[1]._message_id == attempts.first[2]._message_id);
  assert(attempts.second[1] - attempts.second[0] >= 70ms);
  assert(attempts.second[2] - attempts.second[1] >= 170ms);
  assert(wait_until([&] { return pending_file_count(root.path()) == 0U; }));
}

/*
 * 进程退出前没有收到 ACK 时文件必须保留。下一次创建 Publisher 后应自动
 * 扫描旧 pending 文件并发送，不要求业务线程再次调用 enqueue()。
 */
void test_restart_recovers_and_delivers_pending_alarm() {
  TemporaryDirectory root("reliable-restart");
  const std::uint16_t port = reserve_unused_loopback_port();

  {
    ReliableAlarmPublisher publisher(make_config(root.path(), port));
    assert(publisher.enqueue(make_record(40U)).durable());
  }
  assert(pending_file_count(root.path()) == 1U);

  LoopbackTcpListener collector(port);
  auto server = std::async(std::launch::async, [&collector] {
    UniqueSocket client = collector.accept_for(3s);
    auto alarm = client.get() >= 0 ? receive_frame(client.get()) : std::nullopt;
    if (!alarm.has_value() ||
        !send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            *alarm)) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }
    return alarm;
  });

  ReliableAlarmPublisher publisher(make_config(root.path(), port));
  const auto recovered = server.get();
  assert(recovered.has_value());
  assert(recovered->_source_id == SOURCE_ID);
  assert(wait_until([&] { return pending_file_count(root.path()) == 0U; }));
}

/*
 * 成功清空 outbox 后连接保持空闲，Publisher 应发送 PING；Collector 返回
 * 同身份 PONG 后连接仍可继续使用。
 */
void test_idle_connection_uses_ping_pong() {
  TemporaryDirectory root("reliable-heartbeat");
  LoopbackTcpListener collector;

  auto server = std::async(std::launch::async, [&collector] {
    UniqueSocket client = collector.accept_for(3s);
    if (client.get() < 0) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }

    auto alarm = receive_frame(client.get());
    if (!alarm.has_value() ||
        !send_control_frame(client.get(), AlarmWire::AlarmFrameType::ACK,
                            *alarm)) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }

    auto ping = receive_frame(client.get(), 2s);
    if (!ping.has_value() ||
        ping->_type != AlarmWire::AlarmFrameType::PING ||
        !send_control_frame(client.get(), AlarmWire::AlarmFrameType::PONG,
                            *ping)) {
      return std::optional<AlarmWire::AlarmFrame>{};
    }
    return ping;
  });

  ReliableAlarmPublisher publisher(make_config(root.path(), collector.port()));
  assert(publisher.enqueue(make_record(50U)).durable());

  const auto ping = server.get();
  assert(ping.has_value());
  assert(ping->_type == AlarmWire::AlarmFrameType::PING);
  assert(ping->_source_id == SOURCE_ID);
  assert(ping->_payload.empty());
}

/*
 * 多个业务线程可以同时 enqueue()。每条成功结果必须对应一个独立、合法、
 * message ID 唯一的 ALARM 文件，且不能产生临时文件残留。
 */
void test_concurrent_enqueue_creates_unique_durable_alarms() {
  TemporaryDirectory root("reliable-concurrent");
  const std::uint16_t port = reserve_unused_loopback_port();
  constexpr std::size_t thread_count = 16U;
  std::array<AlarmEnqueueStatus, thread_count> statuses{};

  {
    ReliableAlarmPublisher publisher(make_config(root.path(), port));
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t i = 0U; i < thread_count; ++i) {
      workers.emplace_back([&, i] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        statuses[i] =
            publisher.enqueue(make_record(static_cast<std::uint32_t>(100U + i)))
                ._status;
      });
    }

    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
      worker.join();
    }
  }

  assert(std::all_of(statuses.begin(), statuses.end(), [](auto status) {
    return status == AlarmEnqueueStatus::SUCCESS;
  }));

  AlarmWire::AlarmSpool spool(
      {root.path(), AlarmWire::AlarmSpoolKind::OUTBOX, 0U});
  assert(spool.stats()._stats._files == thread_count);

  std::set<AlarmWire::AlarmMessageId> message_ids;
  std::set<std::uint32_t> event_ids;

  while (true) {
    const auto next = spool.next();
    if (next._status == AlarmWire::AlarmSpoolStatus::EMPTY) {
      break;
    }
    assert(next.success());

    const auto stored = spool.read(next._path);
    assert(stored.success());
    const auto alarm = AlarmWire::decode_alarm_frame(
        stored._bytes.data(), stored._bytes.size());
    assert(alarm.success());
    message_ids.insert(alarm._frame->_message_id);

    const auto payload = Wire::decode_v2(alarm._frame->_payload.data(),
                                         alarm._frame->_payload.size());
    assert(payload._status == Wire::WireStatus::SUCCESS);
    event_ids.insert(payload._record->_data._key._eid);
    assert(spool.remove(next._path).success());
  }

  assert(message_ids.size() == thread_count);
  assert(event_ids.size() == thread_count);
  assert(regular_file_count(root.path() / "tmp") == 0U);
}

/*
 * 后台线程阻塞等待 ACK 时析构必须主动 shutdown socket 并 join，不能等待完整
 * ack_timeout；未确认文件仍留在 outbox，供下一次启动恢复。
 */
void test_destructor_wakes_ack_waiter_and_preserves_pending() {
  TemporaryDirectory root("reliable-destructor");
  LoopbackTcpListener collector;
  std::atomic<bool> alarm_received{false};

  auto server = std::async(std::launch::async, [&] {
    UniqueSocket client = collector.accept_for(3s);
    if (client.get() < 0 || !receive_frame(client.get()).has_value()) {
      return false;
    }

    alarm_received.store(true, std::memory_order_release);
    std::array<std::uint8_t, 1U> byte{};
    const ssize_t result = ::recv(client.get(), byte.data(), byte.size(), 0);
    return result == 0;
  });

  auto publisher = std::make_unique<ReliableAlarmPublisher>(
      ReliableAlarmPublisherConfig{"127.0.0.1", collector.port(), SOURCE_ID,
                                   root.path(), 0U, 5s, 250ms});
  assert(publisher->enqueue(make_record(200U)).durable());
  assert(wait_until(
      [&] { return alarm_received.load(std::memory_order_acquire); }));

  const auto start = std::chrono::steady_clock::now();
  publisher.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  assert(elapsed < 1s);
  assert(server.get());
  assert(pending_file_count(root.path()) == 1U);
}

} // namespace

int main() {
  test_configuration_and_setup_status();
  test_enqueue_persists_complete_v2_alarm_without_collector();
  test_enqueue_rejects_encoding_errors_and_full_outbox();
  test_matching_ack_removes_pending_alarm();
  test_engine_report_error_reaches_reliable_publisher();
  test_mismatched_ack_keeps_pending_alarm();
  test_only_one_alarm_is_in_flight();
  test_disconnect_retries_with_exponential_backoff();
  test_restart_recovers_and_delivers_pending_alarm();
  test_idle_connection_uses_ping_pong();
  test_concurrent_enqueue_creates_unique_durable_alarms();
  test_destructor_wakes_ack_waiter_and_preserves_pending();

  std::cout << "M9_RELIABLE_ALARM_PUBLISHER=PASS\n";
  return 0;
}
