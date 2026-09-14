#include "alarm_protocol.h"
#include "alarm_spool.h"
#include "alarm_stream_decoder.h"
#include "monitor_collector.h"
#include "monitor_wire.h"
#include "reliable_alarm_collector.h"
#include "wake_pipe.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace TLSSMON {

namespace {

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr std::size_t MAX_ACCEPTS_PER_ROUND = 8U;
constexpr std::size_t MAX_FRAMES_PER_CLIENT_ROUND = 8U;

/*
 * 最小合法控制帧为 48 字节。
 *
 * 每轮最多读取 8 × 48 字节，因此即使收到连续 PING，
 * 一轮最多也只会完成 8 个帧，防止单客户端独占工作线程。
 */
constexpr std::size_t CLIENT_READ_CHUNK =
    AlarmWire::ALARM_HEADER_SIZE * MAX_FRAMES_PER_CLIENT_ROUND;

class UniqueFd final {
public:
  explicit UniqueFd(int fd = -1) noexcept : _fd(fd) {}

  ~UniqueFd() {
    if (_fd >= 0) {
      (void)::close(_fd);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : _fd(other.release()) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0) {
        (void)::close(_fd);
      }

      _fd = other.release();
    }

    return *this;
  }

  [[nodiscard]]
  int get() const noexcept {
    return _fd;
  }

  [[nodiscard]]
  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

private:
  int _fd{-1};
};

bool set_nonblocking_and_cloexec(int fd) noexcept {
  const int status_flags = ::fcntl(fd, F_GETFL, 0);

  if (status_flags < 0 ||
      ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
    return false;
  }

  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);

  return descriptor_flags >= 0 &&
         ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

bool valid_config(const ReliableAlarmCollectorConfig &config) noexcept {

  return !config._bind_host.empty() && !config._inbox_dir.empty() &&
         config._max_clients != 0U &&
         config._client_timeout > std::chrono::milliseconds::zero();
}

bool engine_accepts_recovery(const Engine &engine) noexcept {
  const EnginePhase phase = engine.get_phase();

  return phase == EnginePhase::READY || phase == EnginePhase::RUNNING;
}

struct AlarmIdentity final {
  std::uint64_t _source_id{0U};
  AlarmWire::AlarmMessageId _message_id{};
};

bool operator<(const AlarmIdentity &lhs, const AlarmIdentity &rhs) noexcept {
  return std::tie(lhs._source_id, lhs._message_id) <
         std::tie(rhs._source_id, rhs._message_id);
}

AlarmIdentity identity_of(const AlarmWire::AlarmFrame &frame) noexcept {

  return AlarmIdentity{frame._source_id, frame._message_id};
}

bool ingest_succeeded(MonData::UpdateStatus status) noexcept {

  return status == MonData::UpdateStatus::INSERTED ||
         status == MonData::UpdateStatus::UPDATED ||
         status == MonData::UpdateStatus::UNCHANGED;
}

std::optional<Wire::DecodedRecord>
decode_reliable_payload(const AlarmWire::AlarmFrame &frame) {

  if (frame._type != AlarmWire::AlarmFrameType::ALARM ||
      frame._payload.empty()) {
    return std::nullopt;
  }

  /*
   * 可靠链路固定使用 V2。
   *
   * 这里不能调用统一 decode() 后接受 V1，因为可靠告警需要保留
   * description 和生产端 changed_at。
   */
  Wire::DecodeResult decoded =
      Wire::decode_v2(frame._payload.data(), frame._payload.size());

  if (decoded._status != Wire::WireStatus::SUCCESS ||
      !decoded._record.has_value() ||
      decoded._record->_version != Wire::WireVersion::V2 ||
      !decoded._record->_changed_at.has_value()) {
    return std::nullopt;
  }

  return std::move(decoded._record);
}

int poll_timeout(const std::vector<Deadline> &deadlines) noexcept {

  if (deadlines.empty()) {
    return -1;
  }

  const Deadline earliest =
      *std::min_element(deadlines.begin(), deadlines.end());

  const Deadline now = Clock::now();

  if (now >= earliest) {
    return 0;
  }

  const Clock::duration remaining = earliest - now;

  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);

  if (milliseconds < remaining) {
    milliseconds += std::chrono::milliseconds{1};
  }

  if (milliseconds.count() > INT_MAX) {
    return INT_MAX;
  }

  return static_cast<int>(milliseconds.count());
}

} // namespace

struct ReliableAlarmCollector::Impl final {
  struct PendingWrite final {
    std::vector<std::uint8_t> _bytes;
    std::size_t _offset{0U};
  };

  struct Client final {
    explicit Client(int input_fd)
        : _fd(input_fd), _last_activity(Clock::now()) {}

    int _fd{-1};
    AlarmWire::AlarmStreamDecoder _decoder;
    std::deque<PendingWrite> _writes;
    Clock::time_point _last_activity;
    bool _read_closed{false};
    bool _close{false};
  };

  struct RecoveryItem final {
    fs::path _path;
    AlarmWire::AlarmFrame _frame;
    Wire::DecodedRecord _record;
  };

  Impl(Engine &input_engine, const ReliableAlarmCollectorConfig &input_config)
      : _engine(input_engine), _config(input_config) {}

  ~Impl() { stop(); }

  bool initialize();
  bool recover_inbox();
  bool collect_recovery_paths(std::vector<fs::path> &paths);
  bool open_listener();
  void run() noexcept;
  bool poll_once();
  void accept_clients();
  bool read_client(Client &client);
  bool write_client(Client &client);
  bool process_frame(Client &client, AlarmWire::AlarmFrame frame);
  bool process_alarm(Client &client, AlarmWire::AlarmFrame frame);
  bool queue_control(Client &client, AlarmWire::AlarmFrameType type,
                     const AlarmWire::AlarmFrame &request);

  std::optional<Wire::DecodedRecord>
  load_persisted_record(const fs::path &path);

  void close_marked_clients() noexcept;
  void close_all_clients() noexcept;
  void close_listener() noexcept;
  void stop() noexcept;
  ReliableAlarmCollectorStatus status() const;
  void record_stream_failure(
      const AlarmWire::AlarmStreamFeedResult &result) noexcept;
  void record_protocol_error() noexcept;
  void record_persist_failure() noexcept;
  void record_duplicate() noexcept;
  void record_accepted() noexcept;
  void record_rejected_client() noexcept;
  void update_client_count() noexcept;

  Engine &_engine;
  const ReliableAlarmCollectorConfig &_config;
  std::unique_ptr<AlarmWire::AlarmSpool> _spool;
  WakeupPipe _wakeup;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _active{false};
  int _listener{-1};
  std::uint16_t _bound_port{0U};
  std::vector<std::unique_ptr<Client>> _clients;
  /*
   * 只由构造阶段恢复逻辑和单个网络线程访问。
   */
  std::set<AlarmIdentity> _accepted;
  std::thread _worker;
  ReliableAlarmCollectorSetupStatus _setup_status{
      ReliableAlarmCollectorSetupStatus::INVALID_CONFIG};
  int _setup_error{0};

  mutable std::mutex _status_mutex;

  std::size_t _client_count{0U};
  std::uint64_t _rejected_clients{0U};
  std::uint64_t _accepted_count{0U};
  std::uint64_t _duplicates{0U};
  std::uint64_t _protocol_errors{0U};
  std::uint64_t _crc_errors{0U};
  std::uint64_t _persist_failures{0U};
};

bool ReliableAlarmCollector::Impl::initialize() {
  if (!valid_config(_config)) {
    _setup_status = ReliableAlarmCollectorSetupStatus::INVALID_CONFIG;
    _setup_error = EINVAL;
    return false;
  }

  if (!engine_accepts_recovery(_engine)) {
    _setup_status = ReliableAlarmCollectorSetupStatus::ENGINE_NOT_READY;
    _setup_error = 0;
    return false;
  }

  try {
    _spool =
        std::make_unique<AlarmWire::AlarmSpool>(AlarmWire::AlarmSpoolConfig{
            _config._inbox_dir, AlarmWire::AlarmSpoolKind::INBOX, 0U});
  } catch (const std::bad_alloc &) {
    _setup_status = ReliableAlarmCollectorSetupStatus::SPOOL_ERROR;
    _setup_error = ENOMEM;
    return false;
  }

  if (!_spool->ready()) {
    _setup_status = ReliableAlarmCollectorSetupStatus::SPOOL_ERROR;
    _setup_error = _spool->setup_error();
    return false;
  }

  try {
    if (!recover_inbox()) {
      _setup_status = ReliableAlarmCollectorSetupStatus::RECOVERY_ERROR;

      if (_setup_error == 0) {
        _setup_error = EIO;
      }

      return false;
    }
  } catch (const std::bad_alloc &) {
    _setup_status = ReliableAlarmCollectorSetupStatus::RECOVERY_ERROR;
    _setup_error = ENOMEM;
    return false;
  } catch (const fs::filesystem_error &error) {
    _setup_status = ReliableAlarmCollectorSetupStatus::RECOVERY_ERROR;
    _setup_error = error.code().value();
    return false;
  } catch (...) {
    _setup_status = ReliableAlarmCollectorSetupStatus::RECOVERY_ERROR;
    _setup_error = EIO;
    return false;
  }

  if (_wakeup.init() != PIPESTATUS::SUCCESSFUL) {
    _setup_status = ReliableAlarmCollectorSetupStatus::WAKEUP_ERROR;
    _setup_error = EIO;
    return false;
  }

  if (!open_listener()) {
    _setup_status = ReliableAlarmCollectorSetupStatus::SOCKET_ERROR;

    if (_setup_error == 0) {
      _setup_error = EADDRNOTAVAIL;
    }

    return false;
  }

  _stop.store(false, std::memory_order_release);
  _active.store(true, std::memory_order_release);

  try {
    _worker = std::thread([this] { run(); });
  } catch (const std::system_error &error) {
    _active.store(false, std::memory_order_release);
    close_listener();

    _setup_status = ReliableAlarmCollectorSetupStatus::THREAD_ERROR;
    _setup_error = error.code().value();
    return false;
  } catch (const std::bad_alloc &) {
    _active.store(false, std::memory_order_release);
    close_listener();

    _setup_status = ReliableAlarmCollectorSetupStatus::THREAD_ERROR;
    _setup_error = ENOMEM;
    return false;
  }

  _setup_status = ReliableAlarmCollectorSetupStatus::SUCCESS;
  _setup_error = 0;
  return true;
}

bool ReliableAlarmCollector::Impl::collect_recovery_paths(
    std::vector<fs::path> &paths) {

  const fs::path accepted_directory = _config._inbox_dir / "accepted";

  std::error_code error;

  fs::recursive_directory_iterator iterator(accepted_directory,
                                            fs::directory_options::none, error);

  const fs::recursive_directory_iterator end;

  if (error) {
    _setup_error = error.value();
    return false;
  }

  while (iterator != end) {
    const fs::file_status status = iterator->symlink_status(error);

    if (error) {
      _setup_error = error.value();
      return false;
    }

    if (fs::is_regular_file(status)) {
      paths.push_back(iterator->path());
    }

    iterator.increment(error);

    if (error) {
      _setup_error = error.value();
      return false;
    }
  }

  return true;
}

bool ReliableAlarmCollector::Impl::recover_inbox() {
  std::vector<fs::path> paths;

  if (!collect_recovery_paths(paths)) {
    return false;
  }

  std::vector<RecoveryItem> records;
  records.reserve(paths.size());

  for (const fs::path &path : paths) {
    AlarmWire::AlarmSpoolReadResult stored = _spool->read(path);

    /*
     * AlarmSpool::read() 已经将损坏文件移入 corrupt/。
     * 单个损坏文件不能阻止其他历史记录恢复。
     */
    if (stored._status == AlarmWire::AlarmSpoolStatus::CORRUPT) {
      continue;
    }

    if (!stored.success()) {
      _setup_error = stored._system_error;
      return false;
    }

    AlarmWire::AlarmDecodeResult frame = AlarmWire::decode_alarm_frame(
        stored._bytes.data(), stored._bytes.size());

    if (!frame.success() ||
        frame._frame->_type != AlarmWire::AlarmFrameType::ALARM) {
      const auto quarantined = _spool->quarantine(stored._path);

      if (!quarantined.success()) {
        _setup_error = quarantined._system_error;
        return false;
      }

      continue;
    }

    auto decoded = decode_reliable_payload(*frame._frame);

    /*
     * 合法 ALARM 信封里携带非 V2 payload，也属于可靠 inbox
     * 损坏数据，隔离后继续恢复其他文件。
     */
    if (!decoded.has_value()) {
      const auto quarantined = _spool->quarantine(stored._path);

      if (!quarantined.success()) {
        _setup_error = quarantined._system_error;
        return false;
      }

      continue;
    }

    records.push_back(RecoveryItem{stored._path, std::move(*frame._frame),
                                   std::move(*decoded)});
  }

  /*
   * 文件名包含随机 message ID，不能用目录遍历顺序恢复。
   */
  std::sort(records.begin(), records.end(),
            [](const RecoveryItem &lhs, const RecoveryItem &rhs) {
              return std::tie(lhs._frame._timestamp_ms, lhs._frame._source_id,
                              lhs._frame._message_id) <
                     std::tie(rhs._frame._timestamp_ms, rhs._frame._source_id,
                              rhs._frame._message_id);
            });

  for (RecoveryItem &item : records) {
    MonData::UpdateResult result =
        ingest_decoded_record(_engine, std::move(item._record), true);

    if (!ingest_succeeded(result._status)) {
      return false;
    }

    _accepted.insert(identity_of(item._frame));
    record_accepted();
  }

  return true;
}

bool ReliableAlarmCollector::Impl::open_listener() {
  const std::string service = std::to_string(_config._listen_port);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo *raw_addresses = nullptr;

  const int address_result = ::getaddrinfo(
      _config._bind_host.c_str(), service.c_str(), &hints, &raw_addresses);

  if (address_result != 0) {
    _setup_error = EADDRNOTAVAIL;
    return false;
  }

  const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(
      raw_addresses, &::freeaddrinfo);

  for (const addrinfo *address = addresses.get(); address != nullptr;
       address = address->ai_next) {

    UniqueFd candidate{::socket(address->ai_family, address->ai_socktype,
                                address->ai_protocol)};

    if (candidate.get() < 0) {
      _setup_error = errno;
      continue;
    }

    const int enabled = 1;

    (void)::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                       static_cast<socklen_t>(sizeof(enabled)));

    if (!set_nonblocking_and_cloexec(candidate.get())) {
      _setup_error = errno;
      continue;
    }

    if (::bind(candidate.get(), address->ai_addr, address->ai_addrlen) != 0) {
      _setup_error = errno;
      continue;
    }

    if (::listen(candidate.get(), static_cast<int>(_config._max_clients)) !=
        0) {
      _setup_error = errno;
      continue;
    }

    sockaddr_storage bound{};
    socklen_t bound_size = static_cast<socklen_t>(sizeof(bound));

    if (::getsockname(candidate.get(), reinterpret_cast<sockaddr *>(&bound),
                      &bound_size) != 0) {
      _setup_error = errno;
      continue;
    }

    if (bound.ss_family == AF_INET) {
      const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&bound);
      _bound_port = ntohs(ipv4->sin_port);
    } else if (bound.ss_family == AF_INET6) {
      const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&bound);
      _bound_port = ntohs(ipv6->sin6_port);
    } else {
      _setup_error = EAFNOSUPPORT;
      continue;
    }

    _listener = candidate.release();
    return true;
  }

  return false;
}

std::optional<Wire::DecodedRecord>
ReliableAlarmCollector::Impl::load_persisted_record(const fs::path &path) {

  AlarmWire::AlarmSpoolReadResult stored = _spool->read(path);

  if (!stored.success()) {
    return std::nullopt;
  }

  AlarmWire::AlarmDecodeResult decoded =
      AlarmWire::decode_alarm_frame(stored._bytes.data(), stored._bytes.size());

  if (!decoded.success()) {
    return std::nullopt;
  }

  auto record = decode_reliable_payload(*decoded._frame);

  if (!record.has_value()) {
    (void)_spool->quarantine(stored._path);
    return std::nullopt;
  }

  return record;
}

bool ReliableAlarmCollector::Impl::queue_control(
    Client &client, AlarmWire::AlarmFrameType type,
    const AlarmWire::AlarmFrame &request) {

  AlarmWire::AlarmFrame response;
  response._type = type;
  response._source_id = request._source_id;
  response._message_id = request._message_id;
  response._timestamp_ms = request._timestamp_ms;

  AlarmWire::AlarmEncodeResult encoded =
      AlarmWire::encode_alarm_frame(response);

  if (!encoded.success()) {
    return false;
  }

  try {
    client._writes.push_back(PendingWrite{std::move(encoded._bytes), 0U});
  } catch (...) {
    return false;
  }

  return true;
}

bool ReliableAlarmCollector::Impl::process_alarm(Client &client,
                                                 AlarmWire::AlarmFrame frame) {

  try {
    const AlarmIdentity identity = identity_of(frame);

    /*
     * 先校验本次收到的 payload。
     *
     * 不能因为身份重复，就对一个损坏 payload 直接 ACK。
     */
    auto incoming_record = decode_reliable_payload(frame);

    if (!incoming_record.has_value()) {
      record_protocol_error();
      return false;
    }

    if (_accepted.find(identity) != _accepted.end()) {
      record_duplicate();
      return queue_control(client, AlarmWire::AlarmFrameType::ACK, frame);
    }

    const AlarmWire::AlarmEncodeResult encoded =
        AlarmWire::encode_alarm_frame(frame);

    if (!encoded.success()) {
      return false;
    }

    const AlarmWire::AlarmSpoolStoreResult stored =
        _spool->store(frame._source_id, frame._message_id, encoded._bytes);

    std::optional<Wire::DecodedRecord> durable_record;
    bool duplicate_on_disk = false;

    if (stored._status == AlarmWire::AlarmSpoolStatus::SUCCESS) {
      durable_record = std::move(incoming_record);
    } else if (stored._status == AlarmWire::AlarmSpoolStatus::DUPLICATE) {
      duplicate_on_disk = true;
      record_duplicate();
      durable_record = load_persisted_record(stored._path);
    } else {
      record_persist_failure();
      return false;
    }

    if (!durable_record.has_value()) {
      record_persist_failure();
      return false;
    }

    MonData::UpdateResult ingested =
        ingest_decoded_record(_engine, std::move(*durable_record), true);

    if (!ingest_succeeded(ingested._status)) {
      /*
       * 文件已经保存，但 Engine 尚未提交。
       * 不记录 _accepted，也不 ACK。
       *
       * Publisher 重试后会进入 DUPLICATE 分支并重新写 Engine。
       */
      return false;
    }

    _accepted.insert(identity);
    if (!duplicate_on_disk) {
      record_accepted();
    }

    return queue_control(client, AlarmWire::AlarmFrameType::ACK, frame);
  } catch (...) {
    return false;
  }
}

bool ReliableAlarmCollector::Impl::process_frame(Client &client,
                                                 AlarmWire::AlarmFrame frame) {

  switch (frame._type) {
  case AlarmWire::AlarmFrameType::ALARM:
    return process_alarm(client, std::move(frame));

  case AlarmWire::AlarmFrameType::PING:
    return queue_control(client, AlarmWire::AlarmFrameType::PONG, frame);

  case AlarmWire::AlarmFrameType::ACK:
  case AlarmWire::AlarmFrameType::PONG:
    record_protocol_error();
    return false;
  }

  return false;
}

bool ReliableAlarmCollector::Impl::read_client(Client &client) {

  std::array<std::uint8_t, CLIENT_READ_CHUNK> buffer{};

  for (;;) {
    const ssize_t received =
        ::recv(client._fd, buffer.data(), buffer.size(), 0);

    if (received > 0) {
      client._last_activity = Clock::now();

      AlarmWire::AlarmStreamFeedResult result = client._decoder.feed(
          buffer.data(), static_cast<std::size_t>(received));

      if (!result.success()) {
        record_stream_failure(result);
        return false;
      }

      /*
       * CLIENT_READ_CHUNK 保证一轮最多完成 8 个最小控制帧。
       */
      if (result._frames.size() > MAX_FRAMES_PER_CLIENT_ROUND) {
        return false;
      }

      for (AlarmWire::AlarmFrame &frame : result._frames) {
        if (!process_frame(client, std::move(frame))) {
          return false;
        }
      }

      return true;
    }

    if (received == 0) {
      AlarmWire::AlarmStreamFeedResult finished = client._decoder.finish();

      if (!finished.success()) {
        record_stream_failure(finished);
        return false;
      }

      client._read_closed = true;
      return true;
    }

    if (errno == EINTR) {
      if (_stop.load(std::memory_order_acquire)) {
        return false;
      }

      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }

    return false;
  }
}

bool ReliableAlarmCollector::Impl::write_client(Client &client) {

  if (client._writes.empty()) {
    return true;
  }

  PendingWrite &write = client._writes.front();

  for (;;) {
    const ssize_t sent =
        ::send(client._fd, write._bytes.data() + write._offset,
               write._bytes.size() - write._offset, MSG_NOSIGNAL);

    if (sent > 0) {
      write._offset += static_cast<std::size_t>(sent);
      client._last_activity = Clock::now();

      if (write._offset == write._bytes.size()) {
        client._writes.pop_front();
      }

      /*
       * 每轮只执行一次成功 send()，防止大输出客户端独占线程。
       */
      return true;
    }

    if (sent < 0 && errno == EINTR) {
      if (_stop.load(std::memory_order_acquire)) {
        return false;
      }

      continue;
    }

    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }

    return false;
  }
}

void ReliableAlarmCollector::Impl::accept_clients() {
  for (std::size_t attempt = 0U; attempt < MAX_ACCEPTS_PER_ROUND; ++attempt) {

    const int client_fd = ::accept(_listener, nullptr, nullptr);

    if (client_fd < 0) {
      if (errno == EINTR) {
        if (_stop.load(std::memory_order_acquire)) {
          return;
        }

        --attempt;
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }

      return;
    }

    UniqueFd candidate(client_fd);

    if (!set_nonblocking_and_cloexec(candidate.get())) {
      continue;
    }

    if (_clients.size() >= _config._max_clients) {
      record_rejected_client();
      continue;
    }

    try {
      _clients.push_back(std::make_unique<Client>(candidate.release()));
      update_client_count();
    } catch (...) {
      return;
    }
  }
}

void ReliableAlarmCollector::Impl::close_marked_clients() noexcept {

  for (const auto &client : _clients) {
    if (client->_close && client->_fd >= 0) {
      (void)::shutdown(client->_fd, SHUT_RDWR);
      (void)::close(client->_fd);
      client->_fd = -1;
    }
  }

  _clients.erase(
      std::remove_if(_clients.begin(), _clients.end(),
                     [](const auto &client) { return client->_close; }),
      _clients.end());
  update_client_count();
}

void ReliableAlarmCollector::Impl::close_all_clients() noexcept {

  for (const auto &client : _clients) {
    if (client->_fd >= 0) {
      (void)::shutdown(client->_fd, SHUT_RDWR);
      (void)::close(client->_fd);
      client->_fd = -1;
    }
  }

  _clients.clear();
  update_client_count();
}

bool ReliableAlarmCollector::Impl::poll_once() {
  std::vector<pollfd> descriptors;
  descriptors.reserve(2U + _clients.size());

  descriptors.push_back(pollfd{_listener, POLLIN, 0});

  descriptors.push_back(pollfd{_wakeup.read_fd(), POLLIN, 0});

  std::vector<Deadline> deadlines;
  deadlines.reserve(_clients.size());

  for (const auto &client : _clients) {
    short events = 0;

    if (!client->_read_closed) {
      events = static_cast<short>(events | POLLIN);
    }

    if (!client->_writes.empty()) {
      events = static_cast<short>(events | POLLOUT);
    }

    descriptors.push_back(pollfd{client->_fd, events, 0});

    deadlines.push_back(client->_last_activity + _config._client_timeout);
  }

  int result;

  for (;;) {
    const int timeout = poll_timeout(deadlines);

    result = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()),
                    timeout);

    if (result >= 0) {
      break;
    }

    if (errno == EINTR) {
      /*
       * deadlines 是绝对时间点。
       * EINTR 后不会重新给予完整 client_timeout。
       */
      if (_stop.load(std::memory_order_acquire)) {
        return false;
      }

      continue;
    }

    return false;
  }

  if ((descriptors[1].revents & POLLIN) != 0) {
    (void)_wakeup.drain();
  }

  if (_stop.load(std::memory_order_acquire)) {
    return false;
  }

  const std::size_t client_count = _clients.size();

  for (std::size_t index = 0U; index < client_count; ++index) {

    Client &client = *_clients[index];

    const short events = descriptors[index + 2U].revents;

    if ((events & POLLIN) != 0 && !client._read_closed) {
      if (!read_client(client)) {
        client._close = true;
      }
    }

    if (!client._close && (events & POLLOUT) != 0) {
      if (!write_client(client)) {
        client._close = true;
      }
    }

    /*
     * POLLIN 与 POLLHUP 可能同时出现。
     * 上面先读取剩余字节，这里再确认 EOF。
     */
    if (!client._close && (events & POLLHUP) != 0 && !client._read_closed) {
      if (!read_client(client)) {
        client._close = true;
      }
    }

    if ((events & (POLLERR | POLLNVAL)) != 0) {
      client._close = true;
    }

    if (!client._close && client._read_closed && client._writes.empty()) {
      client._close = true;
    }

    if (!client._close &&
        Clock::now() - client._last_activity >= _config._client_timeout) {
      client._close = true;
    }
  }

  close_marked_clients();

  if ((descriptors[0].revents & POLLIN) != 0) {
    accept_clients();
  }

  if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    return false;
  }

  return true;
}

void ReliableAlarmCollector::Impl::run() noexcept {
  while (!_stop.load(std::memory_order_acquire)) {
    try {
      if (!poll_once()) {
        break;
      }
    } catch (...) {
      break;
    }
  }

  close_all_clients();
  _active.store(false, std::memory_order_release);
}

void ReliableAlarmCollector::Impl::close_listener() noexcept {

  if (_listener >= 0) {
    (void)::shutdown(_listener, SHUT_RDWR);
    (void)::close(_listener);
    _listener = -1;
  }
}

void ReliableAlarmCollector::Impl::stop() noexcept {
  const bool already_stopping = _stop.exchange(true, std::memory_order_acq_rel);

  _active.store(false, std::memory_order_release);

  if (!already_stopping) {
    (void)_wakeup.wakeup();
  }

  if (_worker.joinable()) {
    _worker.join();
  }

  close_all_clients();
  close_listener();
}

void ReliableAlarmCollector::Impl::record_stream_failure(
    const AlarmWire::AlarmStreamFeedResult &result) noexcept {
  if (result._status != AlarmWire::AlarmStreamStatus::PROTOCOL_ERROR) {
    return;
  }

  std::lock_guard<std::mutex> lock(_status_mutex);

  if (result._protocol_status == AlarmWire::AlarmProtocolStatus::CRC_MISMATCH) {
    ++_crc_errors;
  } else {
    ++_protocol_errors;
  }
}

void ReliableAlarmCollector::Impl::record_protocol_error() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  ++_protocol_errors;
}

void ReliableAlarmCollector::Impl::record_persist_failure() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  ++_persist_failures;
}

void ReliableAlarmCollector::Impl::record_duplicate() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  ++_duplicates;
}

void ReliableAlarmCollector::Impl::record_accepted() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  ++_accepted_count;
}

void ReliableAlarmCollector::Impl::record_rejected_client() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  ++_rejected_clients;
}

void ReliableAlarmCollector::Impl::update_client_count() noexcept {
  std::lock_guard<std::mutex> lock(_status_mutex);
  _client_count = _clients.size();
}

ReliableAlarmCollectorStatus ReliableAlarmCollector::Impl::status() const {
  ReliableAlarmCollectorStatus snapshot;

  snapshot._active = _active.load(std::memory_order_acquire);

  {
    std::lock_guard<std::mutex> lock(_status_mutex);

    snapshot._clients = _client_count;
    snapshot._rejected_clients = _rejected_clients;
    snapshot._accepted = _accepted_count;
    snapshot._duplicates = _duplicates;
    snapshot._protocol_errors = _protocol_errors;
    snapshot._crc_errors = _crc_errors;
    snapshot._persist_failures = _persist_failures;
  }

  /*
   * 同样不能持有状态锁进入 Spool。
   */
  if (!_spool) {
    snapshot._spool_error = ENODEV;
    return snapshot;
  }

  try {
    const AlarmWire::AlarmSpoolStatsResult result = _spool->stats();

    snapshot._spool_stats_available = result.success();
    snapshot._spool_error = result._system_error;

    if (result.success()) {
      snapshot._corrupt_files = result._stats._corrupt_files;
    }
  } catch (const std::bad_alloc &) {
    snapshot._spool_error = ENOMEM;
  } catch (...) {
    snapshot._spool_error = EIO;
  }

  return snapshot;
}

ReliableAlarmCollector::ReliableAlarmCollector(
    Engine &engine, ReliableAlarmCollectorConfig config)
    : _engine(engine), _config(std::move(config)),
      _impl(std::make_unique<Impl>(_engine, _config)) {

  (void)_impl->initialize();
}

ReliableAlarmCollector::~ReliableAlarmCollector() = default;

bool ReliableAlarmCollector::ready() const noexcept {
  return _impl && _impl->_active.load(std::memory_order_acquire) &&
         _impl->_setup_status == ReliableAlarmCollectorSetupStatus::SUCCESS;
}

ReliableAlarmCollectorSetupStatus
ReliableAlarmCollector::setup_status() const noexcept {
  if (!_impl) {
    return ReliableAlarmCollectorSetupStatus::THREAD_ERROR;
  }

  return _impl->_setup_status;
}

int ReliableAlarmCollector::setup_error() const noexcept {
  return _impl ? _impl->_setup_error : ENOMEM;
}

std::uint16_t ReliableAlarmCollector::bound_port() const noexcept {
  return _impl ? _impl->_bound_port : 0U;
}

const ReliableAlarmCollectorConfig &
ReliableAlarmCollector::config() const noexcept {
  return _config;
}

void ReliableAlarmCollector::stop() noexcept {
  if (_impl) {
    _impl->stop();
  }
}

ReliableAlarmCollectorStatus ReliableAlarmCollector::status() const {
  if (!_impl) {
    ReliableAlarmCollectorStatus snapshot;
    snapshot._spool_error = ENOMEM;
    return snapshot;
  }

  return _impl->status();
}

} // namespace TLSSMON
