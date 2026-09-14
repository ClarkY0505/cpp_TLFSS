#include "alarm_protocol.h"
#include "alarm_spool.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
namespace TLSSMON {
namespace AlarmWire {
namespace {
namespace fs = std::filesystem;
class UniqueFd final {
public:
  explicit UniqueFd(int fd = -1) noexcept : _fd(fd) {}
  ~UniqueFd() { ::close(_fd); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  [[nodiscard]]
  int get() const noexcept {
    return _fd;
  }

  [[nodiscard]]
  bool valid() const noexcept {
    return _fd >= 0;
  }

  bool close_now() noexcept {
    if (_fd < 0) {
      return true;
    }

    const int fd = _fd;
    _fd = -1;

    return ::close(fd) == 0;
  }

private:
  int _fd;
};

struct ScanResult final {
  bool _success{true};
  std::uint64_t _files{0U};
  std::uint64_t _bytes{0U};
  std::optional<fs::path> _first;
  int _system_error{0};
};

bool valid_kind(AlarmSpoolKind kind) noexcept {
  return kind == AlarmSpoolKind::OUTBOX || kind == AlarmSpoolKind::INBOX;
}

bool retry_fdatasync(int fd) noexcept {
  for (;;) {
    if (::fdatasync(fd) == 0) {
      return true;
    }

    if (errno != EINTR) {
      return false;
    }
  }
}

bool retry_fsync(int fd) noexcept {
  for (;;) {
    if (::fsync(fd) == 0) {
      return true;
    }

    if (errno != EINTR) {
      return false;
    }
  }
}

bool sync_directory(const fs::path &directory, int &system_error) noexcept {
  const int raw_fd =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (raw_fd < 0) {
    system_error = errno;
    return false;
  }

  UniqueFd fd(raw_fd);

  if (!retry_fsync(fd.get())) {
    system_error = errno;
    return false;
  }

  return true;
}

bool write_all(int fd, const std::uint8_t *data, std::size_t size,
               int &system_error) noexcept {
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t written = ::write(fd, data + offset, size - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }

    if (written < 0 && errno == EINTR) {
      continue;
    }
    system_error = written == 0 ? EIO : errno;
    return false;
  }

  return true;
}

bool read_all(int fd, std::uint8_t *data, std::size_t size,
              int &system_error) noexcept {
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t received = ::read(fd, data + offset, size - offset);
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

bool ensure_secure_directory(const fs::path &path, int &system_error) {
  std::error_code error;

  fs::create_directories(path, error);

  if (error) {
    system_error = error.value();
    return false;
  }

  if (!fs::is_directory(path, error) || error) {
    system_error = error ? error.value() : ENOTDIR;
    return false;
  }

  if (::chmod(path.c_str(), 0700) != 0) {
    system_error = errno;
    return false;
  }

  return true;
}

std::string hex_u64(std::uint64_t value) {
  static constexpr char HEX[] = "0123456789abcdef";

  std::string output(16U, '0');

  for (std::size_t i = 0U; i < output.size(); ++i) {
    const std::size_t index = output.size() - 1U - i;
    output[index] = HEX[value & UINT64_C(0x0f)];
    value >>= 4U;
  }

  return output;
}
std::string hex_message_id(const AlarmMessageId &message_id) {
  static constexpr char HEX[] = "0123456789abcdef";

  std::string output(ALARM_MESSAGE_ID_SIZE * 2U, '0');

  for (std::size_t i = 0U; i < message_id.size(); ++i) {
    output[i * 2U] = HEX[message_id[i] >> 4U];
    output[i * 2U + 1U] = HEX[message_id[i] & 0x0fU];
  }

  return output;
}

std::string alarm_filename(std::uint64_t source_id,
                           const AlarmMessageId &message_id) {
  return hex_u64(source_id) + "-" + hex_message_id(message_id) + ".alarm";
}

/*
 * 严格的判定是否是在路径内的目录
 * 目前不知道是否还有其他界限
 * TODO 后期加入TLSS公共函数中
 * */
bool is_strict_descendant(const fs::path &base, const fs::path &candidate) {
  auto base_iterator = base.begin();
  auto candidate_iterator = candidate.begin();

  for (; base_iterator != base.end(); ++base_iterator, ++candidate_iterator) {
    if (candidate_iterator == candidate.end() ||
        *base_iterator != *candidate_iterator) {
      return false;
    }
  }

  return candidate_iterator != candidate.end();
}

AlarmSpoolStatus resolve_owned_regular_file(const fs::path &base,
                                            const fs::path &input,
                                            fs::path &resolved,
                                            int &system_error) {
  std::error_code error;
  const fs::file_status input_status = fs::symlink_status(input, error);
  if (error) {
    system_error = error.value();
    if (error == std::errc::no_such_file_or_directory) {
      return AlarmSpoolStatus::NOT_FOUND;
    }
    return AlarmSpoolStatus::IO_ERROR;
  }

  /*
   * 即使符号链接最终仍指向 Spool 内部，也拒绝通过符号链接操作，
   * 避免检查和使用之间发生目标切换。
   */
  if (fs::is_symlink(input_status)) {
    return AlarmSpoolStatus::OUTSIDE_ROOT;
  }

  if (!fs::is_regular_file(input_status)) {
    return AlarmSpoolStatus::INVALID_ARGUMENT;
  }

  resolved = fs::canonical(input, error);

  if (error) {
    system_error = error.value();
    return AlarmSpoolStatus::IO_ERROR;
  }

  if (!is_strict_descendant(base, resolved)) {
    return AlarmSpoolStatus::OUTSIDE_ROOT;
  }

  return AlarmSpoolStatus::SUCCESS;
}

ScanResult scan_regular_files(const fs::path &directory) {
  ScanResult result;
  std::error_code error;

  fs::recursive_directory_iterator iterator(
      directory, fs::directory_options::skip_permission_denied, error);
  const fs::recursive_directory_iterator end;

  if (error) {
    result._success = false;
    result._system_error = error.value();
    return result;
  }

  while (iterator != end) {
    const fs::path path = iterator->path();
    const fs::file_status status = iterator->symlink_status(error);
    if (fs::is_regular_file(status)) {
      const std::uintmax_t raw_size = iterator->file_size(error);
      if (error || raw_size > std::numeric_limits<std::uint64_t>::max()) {
        result._success = false;
        result._system_error = error ? error.value() : EOVERFLOW;
        return result;
      }

      const std::uint64_t file_size = static_cast<std::uint64_t>(raw_size);
      if (result._bytes >
          std::numeric_limits<std::uint64_t>::max() - file_size) {
        result._success = false;
        result._system_error = EOVERFLOW;
        return result;
      }

      ++result._files;
      result._bytes += file_size;
      if (!result._first.has_value() ||
          path.native() < result._first->native()) {
        result._first = path;
      }
    }

    iterator.increment(error);

    if (error) {
      result._success = false;
      result._system_error = error.value();
      return result;
    }
  }
  return result;
}

bool move_to_corrupt(const fs::path &source, const fs::path &corrupt_directory,
                     fs::path &target, int &system_error) {
  std::error_code error;

  for (unsigned int suffix = 0U; suffix < 1000U; ++suffix) {
    std::string name = source.filename().string();

    if (suffix != 0U) {
      name += "." + std::to_string(suffix);
    }

    target = corrupt_directory / name;

    if (fs::exists(target, error)) {
      if (error) {
        system_error = error.value();
        return false;
      }

      continue;
    }

    fs::rename(source, target, error);

    if (error) {
      system_error = error.value();
      return false;
    }

    int sync_error = 0;

    if (!sync_directory(corrupt_directory, sync_error) ||
        !sync_directory(source.parent_path(), sync_error)) {
      system_error = sync_error;
      return false;
    }

    return true;
  }

  system_error = EEXIST;
  return false;
}

} // namespace

struct AlarmSpool::Impl final {
  explicit Impl(AlarmSpoolConfig input_config)
      : _config(std::move(input_config)) {}

  bool initialize() {
    if (_config._root.empty() || !valid_kind(_config._kind)) {
      _setup_status = AlarmSpoolStatus::INVALID_ARGUMENT;
      return false;
    }
    if (!ensure_secure_directory(_config._root, _setup_error)) {
      _setup_status = AlarmSpoolStatus::IO_ERROR;
      return false;
    }

    std::error_code error;
    _root = fs::canonical(_config._root, error);

    if (error) {
      _setup_status = AlarmSpoolStatus::IO_ERROR;
      _setup_error = error.value();
      return false;
    }

    _tmp_directory = _root / "tmp";
    _corrupt_directory = _root / "corrupt";

    _data_directory =
        _root /
        (_config._kind == AlarmSpoolKind::OUTBOX ? "pending" : "accepted");

    if (!ensure_secure_directory(_tmp_directory, _setup_error) ||
        !ensure_secure_directory(_corrupt_directory, _setup_error) ||
        !ensure_secure_directory(_data_directory, _setup_error)) {
      _setup_status = AlarmSpoolStatus::IO_ERROR;
      return false;
    }

    /*
     * 进程上次可能在 rename() 前崩溃。
     *
     * tmp 文件不能被当作已持久化告警，启动时统一移入 corrupt。
     */
    for (const fs::directory_entry &entry :
         fs::directory_iterator(_tmp_directory, error)) {
      if (error) {
        _setup_status = AlarmSpoolStatus::IO_ERROR;
        _setup_error = error.value();
        return false;
      }

      const fs::file_status status = entry.symlink_status(error);

      if (error) {
        _setup_status = AlarmSpoolStatus::IO_ERROR;
        _setup_error = error.value();
        return false;
      }

      if (!fs::is_regular_file(status)) {
        continue;
      }

      fs::path quarantined;
      int move_error = 0;

      if (!move_to_corrupt(entry.path(), _corrupt_directory, quarantined,
                           move_error)) {
        _setup_status = AlarmSpoolStatus::IO_ERROR;
        _setup_error = move_error;
        return false;
      }
    }

    _setup_status = AlarmSpoolStatus::SUCCESS;
    _ready = true;
    return true;
  }

  AlarmSpoolConfig _config;
  fs::path _root;
  fs::path _data_directory;
  fs::path _tmp_directory;
  fs::path _corrupt_directory;

  mutable std::mutex _mutex;

  bool _ready{false};
  AlarmSpoolStatus _setup_status{AlarmSpoolStatus::NOT_READY};
  int _setup_error{0};
  std::uint64_t _temporary_sequence{0U};
};

AlarmSpool::AlarmSpool(AlarmSpoolConfig config)
    : _impl(std::make_unique<Impl>(std::move(config))) {
  (void)_impl->initialize();
}

AlarmSpool::~AlarmSpool() = default;

bool AlarmSpool::ready() const noexcept { return _impl && _impl->_ready; }

AlarmSpoolStatus AlarmSpool::setup_status() const noexcept {
  return _impl ? _impl->_setup_status : AlarmSpoolStatus::NOT_READY;
}

int AlarmSpool::setup_error() const noexcept {
  return _impl ? _impl->_setup_error : 0;
}

const AlarmSpoolConfig &AlarmSpool::config() const noexcept {
  return _impl->_config;
}

AlarmSpoolStoreResult AlarmSpool::store(std::uint64_t source_id,
                                        const AlarmMessageId &message_id,
                                        const std::vector<std::uint8_t> &wire) {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, setup_error()};
  }

  if (wire.empty() || wire.size() > ALARM_MAX_FRAME_SIZE) {
    return {AlarmSpoolStatus::INVALID_ARGUMENT, {}, 0};
  }

  /*
   * Spool 只保存完整、合法的 ALARM 帧。
   */
  const AlarmDecodeResult decoded =
      decode_alarm_frame(wire.data(), wire.size());

  if (!decoded.success() || decoded._frame->_type != AlarmFrameType::ALARM) {
    return {AlarmSpoolStatus::INVALID_FRAME, {}, 0};
  }

  if (decoded._frame->_source_id != source_id ||
      decoded._frame->_message_id != message_id) {
    return {AlarmSpoolStatus::INVALID_ARGUMENT, {}, 0};
  }
  std::lock_guard<std::mutex> guard(_impl->_mutex);

  int system_error = 0;

  fs::path parent = _impl->_data_directory;

  if (_impl->_config._kind == AlarmSpoolKind::INBOX) {
    parent /= hex_u64(source_id);

    if (!ensure_secure_directory(parent, system_error)) {
      return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
    }
  }

  const fs::path final_path = parent / alarm_filename(source_id, message_id);

  std::error_code error;

  const bool already_exists = fs::exists(final_path, error);
  if (error) {
    return {AlarmSpoolStatus::IO_ERROR, {}, error.value()};
  }

  /*
   * DUPLICATE 要先于 FULL 判断。
   *
   * 即使 Spool 已满，相同消息也已经满足持久化要求。
   */
  if (already_exists) {
    return {AlarmSpoolStatus::DUPLICATE, final_path, 0};
  }

  if (_impl->_config._max_bytes != 0U) {
    const ScanResult scan = scan_regular_files(_impl->_data_directory);

    if (!scan._success) {
      return {AlarmSpoolStatus::IO_ERROR, {}, scan._system_error};
    }
    const std::uint64_t max_bytes = _impl->_config._max_bytes;

    if (scan._bytes > max_bytes || wire.size() > max_bytes - scan._bytes) {
      return {AlarmSpoolStatus::FULL, {}, 0};
    }
  }

  fs::path temporary_path;
  int raw_fd = -1;

  for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
    ++_impl->_temporary_sequence;

    temporary_path = _impl->_tmp_directory /
                     (std::to_string(static_cast<long long>(::getpid())) + "-" +
                      std::to_string(_impl->_temporary_sequence) + ".tmp");
    raw_fd = ::open(temporary_path.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

    if (raw_fd >= 0) {
      break;
    }

    if (errno != EEXIST) {
      return {AlarmSpoolStatus::IO_ERROR, {}, errno};
    }
  }

  if (raw_fd < 0) {
    return {AlarmSpoolStatus::IO_ERROR, {}, EEXIST};
  }

  UniqueFd fd(raw_fd);
  const auto remove_temporary = [&]() noexcept {
    (void)::unlink(temporary_path.c_str());
  };

  if (!write_all(fd.get(), wire.data(), wire.size(), system_error)) {
    remove_temporary();
    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }

  if (!retry_fdatasync(fd.get())) {
    system_error = errno;
    remove_temporary();
    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }

  if (!fd.close_now()) {
    system_error = errno;
    remove_temporary();
    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }
  fs::rename(temporary_path, final_path, error);

  if (error) {
    remove_temporary();
    return {AlarmSpoolStatus::IO_ERROR, {}, error.value()};
  }

  /*
   * rename() 同时修改了目标目录和 tmp 目录。
   */
  if (!sync_directory(parent, system_error) ||
      !sync_directory(_impl->_tmp_directory, system_error)) {
    /*
     * 无法确认目录项是否可靠落盘，因此不把它报告为成功。
     */
    (void)::unlink(final_path.c_str());

    int ignored = 0;
    (void)sync_directory(parent, ignored);

    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }

  return {AlarmSpoolStatus::SUCCESS, final_path, 0};
}

AlarmSpoolPathResult AlarmSpool::next() const {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, setup_error()};
  }

  std::lock_guard<std::mutex> guard(_impl->_mutex);

  const ScanResult scan = scan_regular_files(_impl->_data_directory);

  if (!scan._success) {
    return {AlarmSpoolStatus::IO_ERROR, {}, scan._system_error};
  }

  if (!scan._first.has_value()) {
    return {AlarmSpoolStatus::EMPTY, {}, 0};
  }

  return {AlarmSpoolStatus::SUCCESS, *scan._first, 0};
}

AlarmSpoolReadResult AlarmSpool::read(const fs::path &path) {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, {}, setup_error()};
  }

  std::lock_guard<std::mutex> guard(_impl->_mutex);

  fs::path resolved;
  int system_error = 0;

  const AlarmSpoolStatus ownership = resolve_owned_regular_file(
      _impl->_data_directory, path, resolved, system_error);

  if (ownership != AlarmSpoolStatus::SUCCESS) {
    return {ownership, {}, {}, system_error};
  }

  const int raw_fd =
      ::open(resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

  if (raw_fd < 0) {
    return {AlarmSpoolStatus::IO_ERROR, {}, {}, errno};
  }

  UniqueFd fd(raw_fd);

  struct stat metadata {};

  if (::fstat(fd.get(), &metadata) != 0) {
    return {AlarmSpoolStatus::IO_ERROR, {}, {}, errno};
  }
  bool corrupt =
      !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
      static_cast<std::uint64_t>(metadata.st_size) > ALARM_MAX_FRAME_SIZE;

  std::vector<std::uint8_t> bytes;

  if (!corrupt) {
    bytes.resize(static_cast<std::size_t>(metadata.st_size));

    if (!read_all(fd.get(), bytes.data(), bytes.size(), system_error)) {
      return {AlarmSpoolStatus::IO_ERROR, {}, {}, system_error};
    }

    const AlarmDecodeResult decoded =
        decode_alarm_frame(bytes.data(), bytes.size());

    corrupt =
        !decoded.success() || decoded._frame->_type != AlarmFrameType::ALARM;
  }
  if (corrupt) {
    (void)fd.close_now();

    fs::path corrupt_path;

    if (!move_to_corrupt(resolved, _impl->_corrupt_directory, corrupt_path,
                         system_error)) {
      return {AlarmSpoolStatus::IO_ERROR, {}, {}, system_error};
    }

    return {AlarmSpoolStatus::CORRUPT, corrupt_path, {}, 0};
  }

  return {AlarmSpoolStatus::SUCCESS, resolved, std::move(bytes), 0};
}

AlarmSpoolPathResult AlarmSpool::remove(const fs::path &path) {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, setup_error()};
  }

  std::lock_guard<std::mutex> guard(_impl->_mutex);

  fs::path resolved;
  int system_error = 0;

  const AlarmSpoolStatus ownership = resolve_owned_regular_file(
      _impl->_data_directory, path, resolved, system_error);

  if (ownership != AlarmSpoolStatus::SUCCESS) {
    return {ownership, {}, system_error};
  }
  if (::unlink(resolved.c_str()) != 0) {
    return {AlarmSpoolStatus::IO_ERROR, {}, errno};
  }

  if (!sync_directory(resolved.parent_path(), system_error)) {
    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }

  return {AlarmSpoolStatus::SUCCESS, resolved, 0};
}

AlarmSpoolPathResult AlarmSpool::quarantine(const fs::path &path) {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, setup_error()};
  }

  std::lock_guard<std::mutex> guard(_impl->_mutex);

  fs::path resolved;
  int system_error = 0;

  const AlarmSpoolStatus ownership = resolve_owned_regular_file(
      _impl->_data_directory, path, resolved, system_error);

  if (ownership != AlarmSpoolStatus::SUCCESS) {
    return {ownership, {}, system_error};
  }

  fs::path target;
  if (!move_to_corrupt(resolved, _impl->_corrupt_directory, target,
                       system_error)) {
    return {AlarmSpoolStatus::IO_ERROR, {}, system_error};
  }

  return {AlarmSpoolStatus::SUCCESS, target, 0};
}

AlarmSpoolStatsResult AlarmSpool::stats() const {
  if (!ready()) {
    return {AlarmSpoolStatus::NOT_READY, {}, setup_error()};
  }

  std::lock_guard<std::mutex> guard(_impl->_mutex);

  const ScanResult active = scan_regular_files(_impl->_data_directory);

  if (!active._success) {
    return {AlarmSpoolStatus::IO_ERROR, {}, active._system_error};
  }

  const ScanResult corrupt = scan_regular_files(_impl->_corrupt_directory);

  if (!corrupt._success) {
    return {AlarmSpoolStatus::IO_ERROR, {}, corrupt._system_error};
  }
  return {AlarmSpoolStatus::SUCCESS,
          AlarmSpoolStats{active._files, active._bytes, corrupt._files,
                          corrupt._bytes},
          0};
}

} // namespace AlarmWire
} // namespace TLSSMON
