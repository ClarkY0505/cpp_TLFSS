#include "reliable_alarm_publisher.h"

#include "alarm_protocol.h"
#include "alarm_spool.h"
#include "monitor_wire.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace TLSSMON {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr std::chrono::milliseconds FIRST_BACKOFF{100};
constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL{500};
constexpr unsigned int MESSAGE_ID_ATTEMPTS = 4U;

enum class IoStatus : std::uint8_t { SUCCESS, TIMEOUT, STOPPED, ERROR };
enum class DeliveryStatus : std::uint8_t {
  SUCCESS,
  STOPPED,
  SEND_FAILED,
  ACK_TIMEOUT,
  INVALID_ACK
};

class UniqueFd final {
public:
  explicit UniqueFd(int fd = -1) noexcept : _fd(fd) {}

  ~UniqueFd() {
    if (_fd >= 0) {
      ::close(_fd);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  int get() const noexcept { return _fd; }

  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

private:
  int _fd;
};

Deadline make_deadline(std::chrono::milliseconds timeout) noexcept {

  const Deadline now = Clock::now();
  const auto maximum = Deadline::max() - now;

  if (timeout >= maximum) {
    return Deadline::max();
  }

  return now + timeout;
}

/*
 * 每次 EINTR 后重新计算剩余时间，防止信号持续到来时
 * timeout 被重复重置。
 */
int remaining_timeout_ms(Deadline deadline) noexcept {
  const Deadline now = Clock::now();

  if (now >= deadline) {
    return 0;
  }

  const Clock::duration remaining = deadline - now;

  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);

  /*
   * poll() 使用整数毫秒。向上取整，避免不足 1ms 时提前超时。
   */
  if (milliseconds < remaining) {
    milliseconds += std::chrono::milliseconds{1};
  }

  if (milliseconds.count() > INT_MAX) {
    return INT_MAX;
  }

  return static_cast<int>(milliseconds.count());
}

bool valid_config(const ReliableAlarmPublisherConfig &config) noexcept {

  return !config._collector_host.empty() && config._collector_port != 0U &&
         config._source_id != 0U && !config._outbox_dir.empty() &&
         config._ack_timeout > std::chrono::milliseconds::zero() &&
         config._max_backoff >= FIRST_BACKOFF;
}

/*
 * 不允许退化到 rand()、时间戳或进程号。
 */
bool secure_random_message_id(AlarmWire::AlarmMessageId &message_id,
                              int &system_error) noexcept {

  std::size_t offset = 0U;

  while (offset < message_id.size()) {
    const ssize_t received =
        ::getrandom(message_id.data() + offset, message_id.size() - offset, 0);

    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }

    if (received < 0 && errno == EINTR) {
      continue;
    }

    system_error = received == 0 ? EIO : errno;
    return false;
  }

  return true;
}

std::optional<std::uint64_t>
timestamp_milliseconds(MonData::MonitorTimestamp timestamp) noexcept {

  const auto duration = timestamp.time_since_epoch();

  if (duration < MonData::MonitorTimestamp::duration::zero()) {
    return std::nullopt;
  }

  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(duration);

  return static_cast<std::uint64_t>(milliseconds.count());
}

} // namespace

struct ReliableAlarmPublisher::Impl final {
  explicit Impl(const ReliableAlarmPublisherConfig &input_config)
      : _config(input_config) {}

  ~Impl() { stop(); }

  bool initialize();

  AlarmEnqueueResult enqueue(MonData::StoredRecord record);

  bool ready() const noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    return _accepting;
  }

  bool stopping() const noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stop;
  }

  void stop() noexcept;
  void run() noexcept;
  void run_iteration();

  IoStatus wait_fd(int fd, short events, Deadline deadline) const noexcept;

  bool connect_collector();
  void close_socket() noexcept;
  int socket_snapshot() const noexcept;

  IoStatus send_all(int fd, const std::uint8_t *data, std::size_t size,
                    Deadline deadline) const noexcept;

  IoStatus receive_all(int fd, std::uint8_t *data, std::size_t size,
                       Deadline deadline) const noexcept;

  struct ReceiveResult final {
    IoStatus _status{IoStatus::ERROR};
    std::optional<AlarmWire::AlarmFrame> _frame;
  };

  ReceiveResult receive_frame(int fd, Deadline deadline) const;

  DeliveryStatus send_alarm_and_wait_ack(const std::vector<std::uint8_t> &wire,
                                         const AlarmWire::AlarmFrame &sent);
  ReliableAlarmPublisherStatus status() const;

  bool heartbeat();

  void wait_for_work();
  bool wait_idle();
  bool wait_backoff();
  void reset_backoff();

  AlarmEnqueueResult
  map_spool_result(const AlarmWire::AlarmSpoolStoreResult &result) const;

  const ReliableAlarmPublisherConfig &_config;

  std::unique_ptr<AlarmWire::AlarmSpool> _spool;

  mutable std::mutex _mutex;
  std::condition_variable _condition;

  bool _accepting{false};
  bool _stop{false};
  bool _work_pending{false};
  bool _connected{false};

  int _socket{-1};

  std::chrono::milliseconds _current_backoff{0};
  std::uint64_t _connect_failures{0U};
  std::uint64_t _send_failures{0U};
  std::uint64_t _ack_timeouts{0U};
  std::uint64_t _acks{0U};

  std::thread _worker;

  ReliableAlarmPublisherSetupStatus _setup_status{
      ReliableAlarmPublisherSetupStatus::INVALID_CONFIG};

  int _setup_error{0};
};

bool ReliableAlarmPublisher::Impl::initialize() {
  // 先让 outbox 可用，再启动网络线程；断线不影响后续本地入队。
  if (!valid_config(_config)) {
    _setup_status = ReliableAlarmPublisherSetupStatus::INVALID_CONFIG;
    _setup_error = EINVAL;
    return false;
  }

  try {
    _spool =
        std::make_unique<AlarmWire::AlarmSpool>(AlarmWire::AlarmSpoolConfig{
            _config._outbox_dir, AlarmWire::AlarmSpoolKind::OUTBOX,
            _config._max_outbox_bytes});
  } catch (const std::bad_alloc &) {
    _setup_status = ReliableAlarmPublisherSetupStatus::SPOOL_ERROR;
    _setup_error = ENOMEM;
    return false;
  }

  if (!_spool->ready()) {
    _setup_status = ReliableAlarmPublisherSetupStatus::SPOOL_ERROR;
    _setup_error = _spool->setup_error();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _accepting = true;
    _stop = false;
  }

  try {
    _worker = std::thread([this] { run(); });
  } catch (const std::system_error &error) {
    std::lock_guard<std::mutex> lock(_mutex);
    _accepting = false;
    _setup_status = ReliableAlarmPublisherSetupStatus::THREAD_ERROR;
    _setup_error = error.code().value();
    return false;
  } catch (const std::bad_alloc &) {
    std::lock_guard<std::mutex> lock(_mutex);
    _accepting = false;
    _setup_status = ReliableAlarmPublisherSetupStatus::THREAD_ERROR;
    _setup_error = ENOMEM;
    return false;
  }

  _setup_status = ReliableAlarmPublisherSetupStatus::SUCCESS;
  return true;
}

ReliableAlarmPublisherStatus ReliableAlarmPublisher::Impl::status() const {
  // 先快照内存计数，再单独扫描 Spool；磁盘统计失败时仍能报告连接状态。
  ReliableAlarmPublisherStatus snapshot;

  snapshot._collector_host = _config._collector_host;
  snapshot._collector_port = _config._collector_port;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    snapshot._active = _accepting;
    snapshot._connected = _connected;
    snapshot._connect_failures = _connect_failures;
    snapshot._send_failures = _send_failures;
    snapshot._ack_timeouts = _ack_timeouts;
    snapshot._acks = _acks;
    snapshot._current_backoff = _current_backoff;
  }

  if (!_spool) {
    snapshot._spool_error = ENODEV;
    return snapshot;
  }

  try {
    const AlarmWire::AlarmSpoolStatsResult result = _spool->stats();

    snapshot._spool_stats_available = result.success();
    snapshot._spool_error = result._system_error;

    if (result.success()) {
      snapshot._pending_files = result._stats._files;
      snapshot._pending_bytes = result._stats._bytes;
      snapshot._corrupt_files = result._stats._corrupt_files;
    }
  } catch (const std::bad_alloc &) {
    snapshot._spool_error = ENOMEM;
  } catch (...) {
    snapshot._spool_error = EIO;
  }

  return snapshot;
}

void ReliableAlarmPublisher::Impl::stop() noexcept {
  int socket = -1;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    _accepting = false;
    _stop = true;
    socket = _socket;
  }

  /*
   * 唤醒阻塞在 poll()/recv() 中的发送线程。
   */
  if (socket >= 0) {
    (void)::shutdown(socket, SHUT_RDWR);
  }

  _condition.notify_all();

  if (_worker.joinable()) {
    _worker.join();
  }

  close_socket();

  /*
   * 不调用 Spool::remove()。
   * 未确认告警保留在 pending/，供下次启动恢复。
   */
}

AlarmEnqueueResult ReliableAlarmPublisher::Impl::map_spool_result(
    const AlarmWire::AlarmSpoolStoreResult &result) const {

  using AlarmWire::AlarmSpoolStatus;

  switch (result._status) {
  case AlarmSpoolStatus::SUCCESS:
    return {AlarmEnqueueStatus::SUCCESS, 0};

  case AlarmSpoolStatus::FULL:
    return {AlarmEnqueueStatus::SPOOL_FULL, 0};

  case AlarmSpoolStatus::NOT_READY:
    return {AlarmEnqueueStatus::NOT_READY, result._system_error};

  case AlarmSpoolStatus::DUPLICATE:
    /*
     * 随机 message ID 碰撞由 enqueue() 重试，
     * 正常不会走到最终映射。
     */
    return {AlarmEnqueueStatus::RANDOM_FAILED, EEXIST};

  case AlarmSpoolStatus::INVALID_ARGUMENT:
  case AlarmSpoolStatus::INVALID_FRAME:
    return {AlarmEnqueueStatus::ENCODE_FAILED, 0};

  case AlarmSpoolStatus::EMPTY:
  case AlarmSpoolStatus::NOT_FOUND:
  case AlarmSpoolStatus::OUTSIDE_ROOT:
  case AlarmSpoolStatus::CORRUPT:
  case AlarmSpoolStatus::IO_ERROR:
    return {AlarmEnqueueStatus::IO_ERROR, result._system_error};
  }

  return {AlarmEnqueueStatus::IO_ERROR, EIO};
}

AlarmEnqueueResult
ReliableAlarmPublisher::Impl::enqueue(MonData::StoredRecord record) {
  // 本路径只做编码与持久化。网络发送交给 worker，成功表示可恢复的入队。
  if (!ready()) {
    return {AlarmEnqueueStatus::NOT_READY, 0};
  }

  try {
    /*
     * 可靠告警固定使用 Monitor Wire V2。
     */
    Wire::EncodeResult payload = Wire::encode_v2(record);

    if (payload._status != Wire::WireStatus::SUCCESS) {
      return {AlarmEnqueueStatus::ENCODE_FAILED, 0};
    }

    const auto timestamp = timestamp_milliseconds(record._changed_at);

    if (!timestamp.has_value()) {
      return {AlarmEnqueueStatus::ENCODE_FAILED, 0};
    }

    AlarmWire::AlarmFrame frame;
    frame._type = AlarmWire::AlarmFrameType::ALARM;
    frame._source_id = _config._source_id;
    frame._timestamp_ms = *timestamp;
    frame._payload = std::move(payload._bytes);

    for (unsigned int attempt = 0U; attempt < MESSAGE_ID_ATTEMPTS; ++attempt) {

      int random_error = 0;

      if (!secure_random_message_id(frame._message_id, random_error)) {
        return {AlarmEnqueueStatus::RANDOM_FAILED, random_error};
      }

      const AlarmWire::AlarmEncodeResult encoded =
          AlarmWire::encode_alarm_frame(frame);

      if (!encoded.success()) {
        return {AlarmEnqueueStatus::ENCODE_FAILED, 0};
      }

      const AlarmWire::AlarmSpoolStoreResult stored =
          _spool->store(frame._source_id, frame._message_id, encoded._bytes);

      /*
       * 128 位随机 ID 撞到已有文件时重新生成。
       */
      if (stored._status == AlarmWire::AlarmSpoolStatus::DUPLICATE) {
        continue;
      }

      const AlarmEnqueueResult result = map_spool_result(stored);

      if (!result.durable()) {
        return result;
      }

      {
        std::lock_guard<std::mutex> lock(_mutex);
        _work_pending = true;
      }

      _condition.notify_one();

      /*
       * 网络线程此时是否已经发送不影响 enqueue 的成功语义。
       * SUCCESS 只表示 outbox 已经持久化。
       */
      return result;
    }

    return {AlarmEnqueueStatus::RANDOM_FAILED, EEXIST};
  } catch (const std::bad_alloc &) {
    return {AlarmEnqueueStatus::ENCODE_FAILED, ENOMEM};
  } catch (...) {
    return {AlarmEnqueueStatus::IO_ERROR, EIO};
  }
}

IoStatus
ReliableAlarmPublisher::Impl::wait_fd(int fd, short events,
                                      Deadline deadline) const noexcept {
  // 使用绝对截止时间，EINTR 或短暂就绪不会重新计满超时。
  for (;;) {
    if (stopping()) {
      return IoStatus::STOPPED;
    }

    const int timeout = remaining_timeout_ms(deadline);

    if (timeout == 0) {
      return IoStatus::TIMEOUT;
    }

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = events;

    const int result = ::poll(&descriptor, 1, timeout);

    if (result > 0) {
      /*
       * POLLIN 与 POLLHUP 可以同时出现，先消费已有数据。
       */
      if ((descriptor.revents & events) != 0) {
        return IoStatus::SUCCESS;
      }

      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return IoStatus::ERROR;
      }

      continue;
    }

    if (result == 0) {
      /*
       * timeout 可能因为 INT_MAX 截断而早于真正 deadline，
       * 所以重新检查绝对时间。
       */
      if (Clock::now() >= deadline) {
        return IoStatus::TIMEOUT;
      }

      continue;
    }

    if (errno == EINTR) {
      /*
       * 不重新生成 deadline。
       */
      continue;
    }

    return IoStatus::ERROR;
  }
}

bool ReliableAlarmPublisher::Impl::connect_collector() {
  const Deadline deadline = make_deadline(_config._ack_timeout);

  const std::string service = std::to_string(_config._collector_port);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *raw_addresses = nullptr;

  const int address_result = ::getaddrinfo(
      _config._collector_host.c_str(), service.c_str(), &hints, &raw_addresses);

  if (address_result != 0) {
    return false;
  }

  const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(
      raw_addresses, &::freeaddrinfo);

  for (const addrinfo *address = addresses.get(); address != nullptr;
       address = address->ai_next) {

    if (stopping() || Clock::now() >= deadline) {
      return false;
    }

    UniqueFd socket{::socket(address->ai_family, address->ai_socktype,
                             address->ai_protocol)};

    if (socket.get() < 0) {
      continue;
    }

    const int old_flags = ::fcntl(socket.get(), F_GETFL, 0);

    if (old_flags < 0 ||
        ::fcntl(socket.get(), F_SETFL, old_flags | O_NONBLOCK) != 0 ||
        ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) != 0) {
      continue;
    }

    int result = ::connect(socket.get(), address->ai_addr, address->ai_addrlen);

    if (result != 0) {
      const int connect_error = errno;

      if (connect_error != EINPROGRESS && connect_error != EALREADY &&
          connect_error != EINTR) {
        continue;
      }

      const IoStatus waited = wait_fd(socket.get(), POLLOUT, deadline);

      if (waited != IoStatus::SUCCESS) {
        continue;
      }

      int socket_error = 0;
      socklen_t error_size = static_cast<socklen_t>(sizeof(socket_error));

      if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                       &error_size) != 0 ||
          socket_error != 0) {
        continue;
      }
    }

    {
      std::lock_guard<std::mutex> lock(_mutex);

      if (_stop) {
        return false;
      }

      _socket = socket.release();
      _connected = true;
    }

    return true;
  }

  return false;
}

int ReliableAlarmPublisher::Impl::socket_snapshot() const noexcept {

  std::lock_guard<std::mutex> lock(_mutex);
  return _socket;
}

void ReliableAlarmPublisher::Impl::close_socket() noexcept {
  int socket = -1;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    socket = _socket;
    _socket = -1;
    _connected = false;
  }

  if (socket >= 0) {
    (void)::shutdown(socket, SHUT_RDWR);
    (void)::close(socket);
  }
}

IoStatus
ReliableAlarmPublisher::Impl::send_all(int fd, const std::uint8_t *data,
                                       std::size_t size,
                                       Deadline deadline) const noexcept {

  std::size_t offset = 0U;

  while (offset < size) {
    if (stopping()) {
      return IoStatus::STOPPED;
    }

    if (Clock::now() >= deadline) {
      return IoStatus::TIMEOUT;
    }

    const ssize_t sent = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);

    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent < 0 && errno == EINTR) {
      continue;
    }

    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {

      const IoStatus waited = wait_fd(fd, POLLOUT, deadline);

      if (waited != IoStatus::SUCCESS) {
        return waited;
      }

      continue;
    }

    return IoStatus::ERROR;
  }

  return IoStatus::SUCCESS;
}

IoStatus
ReliableAlarmPublisher::Impl::receive_all(int fd, std::uint8_t *data,
                                          std::size_t size,
                                          Deadline deadline) const noexcept {

  std::size_t offset = 0U;

  while (offset < size) {
    if (stopping()) {
      return IoStatus::STOPPED;
    }

    if (Clock::now() >= deadline) {
      return IoStatus::TIMEOUT;
    }

    const ssize_t received = ::recv(fd, data + offset, size - offset, 0);

    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }

    if (received == 0) {
      return IoStatus::ERROR;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const IoStatus waited = wait_fd(fd, POLLIN, deadline);

      if (waited != IoStatus::SUCCESS) {
        return waited;
      }

      continue;
    }

    return IoStatus::ERROR;
  }

  return IoStatus::SUCCESS;
}

ReliableAlarmPublisher::Impl::ReceiveResult
ReliableAlarmPublisher::Impl::receive_frame(int fd, Deadline deadline) const {

  std::array<std::uint8_t, AlarmWire::ALARM_HEADER_SIZE> header{};

  IoStatus status = receive_all(fd, header.data(), header.size(), deadline);

  if (status != IoStatus::SUCCESS) {
    return {status, std::nullopt};
  }

  const AlarmWire::AlarmFrameSizeResult size_result =
      AlarmWire::alarm_frame_size(header.data(), header.size());

  if (!size_result.success()) {
    return {IoStatus::ERROR, std::nullopt};
  }

  std::vector<std::uint8_t> wire(*size_result._frame_size);

  std::copy(header.begin(), header.end(), wire.begin());

  const std::size_t payload_size = wire.size() - AlarmWire::ALARM_HEADER_SIZE;

  if (payload_size != 0U) {
    status = receive_all(fd, wire.data() + AlarmWire::ALARM_HEADER_SIZE,
                         payload_size, deadline);

    if (status != IoStatus::SUCCESS) {
      return {status, std::nullopt};
    }
  }

  AlarmWire::AlarmDecodeResult decoded =
      AlarmWire::decode_alarm_frame(wire.data(), wire.size());

  if (!decoded.success()) {
    return {IoStatus::ERROR, std::nullopt};
  }

  return {IoStatus::SUCCESS, std::move(decoded._frame)};
}

DeliveryStatus ReliableAlarmPublisher::Impl::send_alarm_and_wait_ack(
    const std::vector<std::uint8_t> &wire, const AlarmWire::AlarmFrame &sent) {
  // ACK 必须同时匹配类型、来源和消息 ID，才能安全删除 outbox 文件。

  const int socket = socket_snapshot();

  if (socket < 0) {
    return DeliveryStatus::SEND_FAILED;
  }

  /*
   * send 和等待 ACK 共用一个绝对 deadline。
   */
  const Deadline deadline = make_deadline(_config._ack_timeout);
  const IoStatus sent_status =
      send_all(socket, wire.data(), wire.size(), deadline);

  if (sent_status == IoStatus::STOPPED) {
    return DeliveryStatus::STOPPED;
  }

  if (sent_status != IoStatus::SUCCESS) {
    return DeliveryStatus::SEND_FAILED;
  }

  const ReceiveResult received = receive_frame(socket, deadline);

  if (received._status == IoStatus::STOPPED) {
    return DeliveryStatus::STOPPED;
  }

  if (received._status == IoStatus::TIMEOUT) {
    return DeliveryStatus::ACK_TIMEOUT;
  }

  if (received._status != IoStatus::SUCCESS ||
      !received._frame.has_value()) {
    return DeliveryStatus::INVALID_ACK;
  }

  const AlarmWire::AlarmFrame &ack = *received._frame;
  if (ack._type != AlarmWire::AlarmFrameType::ACK ||
      ack._source_id != sent._source_id ||
      ack._message_id != sent._message_id) {
    return DeliveryStatus::INVALID_ACK;
  }

  return DeliveryStatus::SUCCESS;
}

bool ReliableAlarmPublisher::Impl::heartbeat() {
  // 队列空闲但连接仍打开时用 PING/PONG 验证连接可继续复用。
  const int socket = socket_snapshot();

  if (socket < 0) {
    return false;
  }

  AlarmWire::AlarmFrame ping;
  ping._type = AlarmWire::AlarmFrameType::PING;
  ping._source_id = _config._source_id;

  int random_error = 0;

  if (!secure_random_message_id(ping._message_id, random_error)) {
    return false;
  }

  const auto timestamp =
      timestamp_milliseconds(std::chrono::system_clock::now());

  if (!timestamp.has_value()) {
    return false;
  }

  ping._timestamp_ms = *timestamp;

  const AlarmWire::AlarmEncodeResult encoded =
      AlarmWire::encode_alarm_frame(ping);

  if (!encoded.success()) {
    return false;
  }

  const Deadline deadline = make_deadline(_config._ack_timeout);

  if (send_all(socket, encoded._bytes.data(), encoded._bytes.size(),
               deadline) != IoStatus::SUCCESS) {
    return false;
  }

  const ReceiveResult received = receive_frame(socket, deadline);

  if (received._status != IoStatus::SUCCESS || !received._frame.has_value()) {
    return false;
  }

  const AlarmWire::AlarmFrame &pong = *received._frame;

  return pong._type == AlarmWire::AlarmFrameType::PONG &&
         pong._source_id == ping._source_id &&
         pong._message_id == ping._message_id;
}

void ReliableAlarmPublisher::Impl::wait_for_work() {
  std::unique_lock<std::mutex> lock(_mutex);

  _condition.wait(lock, [this] { return _stop || _work_pending; });

  _work_pending = false;
}

bool ReliableAlarmPublisher::Impl::wait_idle() {
  std::unique_lock<std::mutex> lock(_mutex);

  const bool notified = _condition.wait_for(
      lock, HEARTBEAT_INTERVAL, [this] { return _stop || _work_pending; });

  if (_stop) {
    return false;
  }

  if (notified && _work_pending) {
    _work_pending = false;
    return true;
  }

  return false;
}

bool ReliableAlarmPublisher::Impl::wait_backoff() {
  std::unique_lock<std::mutex> lock(_mutex);

  if (_current_backoff == std::chrono::milliseconds::zero()) {
    _current_backoff = FIRST_BACKOFF;
  } else {
    const auto doubled = _current_backoff * 2;

    _current_backoff = std::min(doubled, _config._max_backoff);
  }

  /*
   * 新 enqueue 不能跳过退避，只允许 stop 提前终止等待。
   */
  _condition.wait_for(lock, _current_backoff, [this] { return _stop; });

  return !_stop;
}

void ReliableAlarmPublisher::Impl::reset_backoff() {
  std::lock_guard<std::mutex> lock(_mutex);
  _current_backoff = std::chrono::milliseconds::zero();
}

void ReliableAlarmPublisher::Impl::run_iteration() {
  // 每轮只处理队列首条消息，使发送、ACK、删除保持串行顺序。
  const AlarmWire::AlarmSpoolPathResult next = _spool->next();

  if (next._status == AlarmWire::AlarmSpoolStatus::EMPTY) {
    if (socket_snapshot() < 0) {
      wait_for_work();
      return;
    }

    /*
     * 有新工作时立即回到循环；超时表示连接空闲，
     * 执行一次心跳。
     */
    if (wait_idle()) {
      return;
    }

    if (!stopping() && !heartbeat()) {
      close_socket();
      (void)wait_backoff();
    }

    return;
  }

  if (!next.success()) {
    (void)wait_backoff();
    return;
  }

  AlarmWire::AlarmSpoolReadResult stored = _spool->read(next._path);

  if (stored._status == AlarmWire::AlarmSpoolStatus::CORRUPT) {
    return;
  }

  if (!stored.success()) {
    (void)wait_backoff();
    return;
  }

  AlarmWire::AlarmDecodeResult decoded =
      AlarmWire::decode_alarm_frame(stored._bytes.data(), stored._bytes.size());

  if (!decoded.success() ||
      decoded._frame->_type != AlarmWire::AlarmFrameType::ALARM) {

    (void)_spool->quarantine(next._path);
    return;
  }

  /*
   * 后台线程是串行循环，因此任意时刻最多只有一个
   * 已发送但尚未确认的 ALARM。
   */
  if (socket_snapshot() < 0 && !connect_collector()) {
    {
      std::lock_guard<std::mutex> lock(_mutex);

      /*
       * 正常 stop 导致的连接中止不计入连接失败。
       */
      if (!_stop) {
        ++_connect_failures;
      }
    }

    if (!stopping()) {
      (void)wait_backoff();
    }

    return;
  }

  const DeliveryStatus delivery =
      send_alarm_and_wait_ack(stored._bytes, *decoded._frame);

  if (delivery != DeliveryStatus::SUCCESS) {
    {
      std::lock_guard<std::mutex> lock(_mutex);

      switch (delivery) {
      case DeliveryStatus::ACK_TIMEOUT:
        ++_ack_timeouts;
        break;

      case DeliveryStatus::SEND_FAILED:
      case DeliveryStatus::INVALID_ACK:
        ++_send_failures;
        break;

      case DeliveryStatus::STOPPED:
      case DeliveryStatus::SUCCESS:
        break;
      }
    }

    close_socket();

    if (delivery != DeliveryStatus::STOPPED) {
      (void)wait_backoff();
    }

    return;
  }

  /*
   * 计数实际收到并验证成功的 ACK。
   *
   * 即使后续删除 outbox 文件失败，ACK 本身也确实发生过。
   */
  {
    std::lock_guard<std::mutex> lock(_mutex);
    ++_acks;
  }

  /*
   * 只有 source ID 和 message ID 匹配的 ACK 才能到这里。
   */
  const AlarmWire::AlarmSpoolPathResult removed = _spool->remove(next._path);

  if (!removed.success()) {
    /*
     * 如果文件仍存在，下轮会按至少一次送达语义重发；
     * Collector 应根据 source_id + message_id 去重。
     */
    (void)wait_backoff();
    return;
  }

  reset_backoff();
}

void ReliableAlarmPublisher::Impl::run() noexcept {
  while (!stopping()) {
    try {
      run_iteration();
    } catch (...) {
      close_socket();

      if (!wait_backoff()) {
        break;
      }
    }
  }

  close_socket();
}

ReliableAlarmPublisher::ReliableAlarmPublisher(
    ReliableAlarmPublisherConfig config)
    : _config(std::move(config)), _impl(std::make_unique<Impl>(_config)) {

  (void)_impl->initialize();
}

ReliableAlarmPublisher::~ReliableAlarmPublisher() = default;

bool ReliableAlarmPublisher::ready() const noexcept {
  return _impl && _impl->ready();
}

ReliableAlarmPublisherSetupStatus
ReliableAlarmPublisher::setup_status() const noexcept {
  if (!_impl) {
    return ReliableAlarmPublisherSetupStatus::THREAD_ERROR;
  }

  return _impl->_setup_status;
}

int ReliableAlarmPublisher::setup_error() const noexcept {
  return _impl ? _impl->_setup_error : ENOMEM;
}

const ReliableAlarmPublisherConfig &
ReliableAlarmPublisher::config() const noexcept {
  return _config;
}

AlarmEnqueueResult
ReliableAlarmPublisher::enqueue(MonData::StoredRecord record) {

  if (!_impl) {
    return {AlarmEnqueueStatus::NOT_READY, 0};
  }

  return _impl->enqueue(std::move(record));
}

ReliableAlarmPublisherStatus ReliableAlarmPublisher::status() const {
  if (!_impl) {
    ReliableAlarmPublisherStatus snapshot;
    snapshot._collector_host = _config._collector_host;
    snapshot._collector_port = _config._collector_port;
    snapshot._spool_error = ENOMEM;
    return snapshot;
  }

  return _impl->status();
}

} // namespace TLSSMON
