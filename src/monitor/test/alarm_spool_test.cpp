#include "alarm_protocol.h"
#include "alarm_spool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace TLSSMON::AlarmWire;

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(const std::string &name) {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto tick = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
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

AlarmMessageId make_message_id(std::uint8_t seed) {
  AlarmMessageId id{};
  for (std::size_t i = 0U; i < id.size(); ++i) {
    id[i] = static_cast<std::uint8_t>(seed + i);
  }
  return id;
}

std::vector<std::uint8_t> make_wire(std::uint64_t source_id,
                                    const AlarmMessageId &message_id,
                                    std::size_t payload_size = 32U) {
  AlarmFrame frame;
  frame._type = AlarmFrameType::ALARM;
  frame._source_id = source_id;
  frame._message_id = message_id;
  frame._timestamp_ms = UINT64_C(1700000000123);
  frame._payload.resize(payload_size, 0x5aU);

  const AlarmEncodeResult encoded = encode_alarm_frame(frame);
  assert(encoded.success());
  return encoded._bytes;
}

mode_t permission_bits(const fs::path &path) {
  struct stat metadata {};
  assert(::stat(path.c_str(), &metadata) == 0);
  return metadata.st_mode & static_cast<mode_t>(0777);
}

std::string hex_u64(std::uint64_t value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result(16U, '0');
  for (std::size_t i = 0U; i < result.size(); ++i) {
    const std::size_t index = result.size() - 1U - i;
    result[index] = hex[value & UINT64_C(0x0f)];
    value >>= 4U;
  }
  return result;
}

/*
 * OUTBOX 必须支持保存、扫描、读取、重启恢复和显式删除。
 * 删除前文件始终存在，用来固定“ACK 后才删除”的生命周期边界。
 */
void test_outbox_store_read_reopen_and_remove() {
  TemporaryDirectory root("spool-outbox");
  const std::uint64_t source_id = UINT64_C(0x1122);
  const AlarmMessageId message_id = make_message_id(1U);
  const auto wire = make_wire(source_id, message_id);
  fs::path stored_path;

  {
    AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 4096U});
    assert(spool.ready());
    assert(spool.setup_status() == AlarmSpoolStatus::SUCCESS);
    assert(fs::is_directory(root.path() / "pending"));
    assert(fs::is_directory(root.path() / "tmp"));
    assert(fs::is_directory(root.path() / "corrupt"));

    const AlarmSpoolStoreResult stored =
        spool.store(source_id, message_id, wire);
    assert(stored.success());
    assert(stored.durable());
    stored_path = stored._path;
    assert(fs::exists(stored_path));
    assert(stored_path.parent_path().filename() == "pending");
    assert(stored_path.filename().string().find(hex_u64(source_id)) == 0U);

    const AlarmSpoolStatsResult stats = spool.stats();
    assert(stats.success());
    assert(stats._stats._files == 1U);
    assert(stats._stats._bytes == wire.size());
    assert(stats._stats._corrupt_files == 0U);

    const AlarmSpoolPathResult next = spool.next();
    assert(next.success());
    assert(next._path == stored_path);

    const AlarmSpoolReadResult read = spool.read(next._path);
    assert(read.success());
    assert(read._path == stored_path);
    assert(read._bytes == wire);

    // 没有 ACK/remove() 时，读取不会删除 pending 文件。
    assert(fs::exists(stored_path));
  }

  {
    AlarmSpool reopened({root.path(), AlarmSpoolKind::OUTBOX, 4096U});
    assert(reopened.ready());
    const AlarmSpoolPathResult next = reopened.next();
    assert(next.success());
    assert(next._path == stored_path);
    assert(reopened.remove(next._path).success());
    assert(!fs::exists(stored_path));
    assert(reopened.next()._status == AlarmSpoolStatus::EMPTY);
  }
}

/* DUPLICATE 优先于 FULL；第二个身份才会触发容量上限。 */
void test_duplicate_and_capacity() {
  TemporaryDirectory root("spool-capacity");
  const std::uint64_t source_id = 8U;
  const AlarmMessageId first_id = make_message_id(2U);
  const AlarmMessageId second_id = make_message_id(3U);
  const auto first_wire = make_wire(source_id, first_id);
  const auto second_wire = make_wire(source_id, second_id);

  AlarmSpool spool(
      {root.path(), AlarmSpoolKind::OUTBOX, first_wire.size()});
  assert(spool.ready());
  assert(spool.store(source_id, first_id, first_wire).success());

  const AlarmSpoolStoreResult duplicate =
      spool.store(source_id, first_id, first_wire);
  assert(duplicate._status == AlarmSpoolStatus::DUPLICATE);
  assert(!duplicate.success());
  assert(duplicate.durable());

  const AlarmSpoolStoreResult full =
      spool.store(source_id, second_id, second_wire);
  assert(full._status == AlarmSpoolStatus::FULL);
  assert(!full.durable());
}

/* INBOX 使用 accepted/source-id/ 两级布局，并支持主动隔离。 */
void test_inbox_layout_duplicate_and_quarantine() {
  TemporaryDirectory root("spool-inbox");
  const std::uint64_t source_id = UINT64_C(0x1122);
  const AlarmMessageId message_id = make_message_id(4U);
  const auto wire = make_wire(source_id, message_id);

  AlarmSpool spool({root.path(), AlarmSpoolKind::INBOX, 0U});
  assert(spool.ready());
  const AlarmSpoolStoreResult stored =
      spool.store(source_id, message_id, wire);
  assert(stored.success());
  assert(stored._path.parent_path().filename() == hex_u64(source_id));
  assert(stored._path.parent_path().parent_path().filename() == "accepted");
  assert(spool.store(source_id, message_id, wire)._status ==
         AlarmSpoolStatus::DUPLICATE);

  const AlarmSpoolPathResult quarantined = spool.quarantine(stored._path);
  assert(quarantined.success());
  assert(quarantined._path.parent_path().filename() == "corrupt");
  assert(fs::exists(quarantined._path));
  assert(!fs::exists(stored._path));

  const AlarmSpoolStatsResult stats = spool.stats();
  assert(stats.success());
  assert(stats._stats._files == 0U);
  assert(stats._stats._bytes == 0U);
  assert(stats._stats._corrupt_files == 1U);
}

/* read() 检测 CRC 损坏后必须自动移动文件到 corrupt/。 */
void test_corrupt_file_is_quarantined_on_read() {
  TemporaryDirectory root("spool-corrupt");
  const std::uint64_t source_id = 9U;
  const AlarmMessageId message_id = make_message_id(5U);
  const auto wire = make_wire(source_id, message_id);
  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  const AlarmSpoolStoreResult stored =
      spool.store(source_id, message_id, wire);
  assert(stored.success());

  {
    std::fstream file(stored._path,
                      std::ios::binary | std::ios::in | std::ios::out);
    assert(file.good());
    file.seekp(static_cast<std::streamoff>(ALARM_HEADER_SIZE));
    const char damaged = static_cast<char>(0x33);
    file.write(&damaged, 1);
    assert(file.good());
  }

  const AlarmSpoolReadResult read = spool.read(stored._path);
  assert(read._status == AlarmSpoolStatus::CORRUPT);
  assert(read._bytes.empty());
  assert(read._path.parent_path().filename() == "corrupt");
  assert(fs::exists(read._path));
  assert(!fs::exists(stored._path));
}

/* Spool 外路径和符号链接均不得被读取、删除或隔离。 */
void test_rejects_paths_outside_spool_and_symlinks() {
  TemporaryDirectory root("spool-path");
  TemporaryDirectory outside("spool-outside");
  fs::create_directories(outside.path());
  const fs::path outside_file = outside.path() / "outside.alarm";
  {
    std::ofstream output(outside_file, std::ios::binary);
    output << "outside";
  }

  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  assert(spool.ready());
  assert(spool.read(outside_file)._status == AlarmSpoolStatus::OUTSIDE_ROOT);
  assert(spool.remove(outside_file)._status == AlarmSpoolStatus::OUTSIDE_ROOT);
  assert(spool.quarantine(outside_file)._status ==
         AlarmSpoolStatus::OUTSIDE_ROOT);
  assert(fs::exists(outside_file));

  const fs::path link = root.path() / "pending" / "outside-link.alarm";
  std::error_code error;
  fs::create_symlink(outside_file, link, error);
  assert(!error);
  assert(spool.read(link)._status == AlarmSpoolStatus::OUTSIDE_ROOT);
  assert(fs::exists(outside_file));
}

/* 上次崩溃遗留的 tmp 文件在重新打开时必须移入 corrupt/。 */
void test_restart_quarantines_stale_tmp_file() {
  TemporaryDirectory root("spool-tmp-recovery");
  {
    AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
    assert(spool.ready());
  }

  const fs::path stale = root.path() / "tmp" / "stale.tmp";
  {
    std::ofstream output(stale, std::ios::binary);
    output << "partial frame";
  }
  assert(fs::exists(stale));

  AlarmSpool reopened({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  assert(reopened.ready());
  assert(!fs::exists(stale));
  const AlarmSpoolStatsResult stats = reopened.stats();
  assert(stats.success());
  assert(stats._stats._files == 0U);
  assert(stats._stats._corrupt_files == 1U);
}

/* 固定目录 0700、告警文件 0600 的安全权限。 */
void test_secure_permissions() {
  TemporaryDirectory root("spool-permissions");
  const std::uint64_t source_id = 10U;
  const AlarmMessageId id = make_message_id(6U);
  const auto wire = make_wire(source_id, id);
  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  assert(spool.ready());
  assert(permission_bits(root.path()) == 0700);
  assert(permission_bits(root.path() / "pending") == 0700);
  assert(permission_bits(root.path() / "tmp") == 0700);
  assert(permission_bits(root.path() / "corrupt") == 0700);
  const AlarmSpoolStoreResult stored = spool.store(source_id, id, wire);
  assert(stored.success());
  assert(permission_bits(stored._path) == 0600);
}

/* 同一 Spool 并发保存同一身份：一次 SUCCESS，其余 DUPLICATE。 */
void test_concurrent_duplicate_store() {
  TemporaryDirectory root("spool-concurrency");
  const std::uint64_t source_id = 11U;
  const AlarmMessageId id = make_message_id(7U);
  const auto wire = make_wire(source_id, id);
  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  assert(spool.ready());

  constexpr std::size_t thread_count = 16U;
  std::array<AlarmSpoolStatus, thread_count> statuses{};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (std::size_t i = 0U; i < thread_count; ++i) {
    workers.emplace_back([&, i] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      statuses[i] = spool.store(source_id, id, wire)._status;
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread &worker : workers) {
    worker.join();
  }

  assert(std::count(statuses.begin(), statuses.end(),
                    AlarmSpoolStatus::SUCCESS) == 1);
  assert(std::count(statuses.begin(), statuses.end(),
                    AlarmSpoolStatus::DUPLICATE) == thread_count - 1U);
  assert(spool.stats()._stats._files == 1U);
}

/* 非法 CRC、控制帧和参数身份不一致都不能进入 Spool。 */
void test_rejects_invalid_frames_and_identity_mismatch() {
  TemporaryDirectory root("spool-invalid");
  const std::uint64_t source_id = 12U;
  const AlarmMessageId id = make_message_id(8U);
  auto wire = make_wire(source_id, id);
  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});

  wire[ALARM_HEADER_SIZE] ^= 0x01U;
  assert(spool.store(source_id, id, wire)._status ==
         AlarmSpoolStatus::INVALID_FRAME);

  AlarmFrame ping;
  ping._type = AlarmFrameType::PING;
  ping._source_id = source_id;
  ping._message_id = id;
  const AlarmEncodeResult encoded_ping = encode_alarm_frame(ping);
  assert(encoded_ping.success());
  assert(spool.store(source_id, id, encoded_ping._bytes)._status ==
         AlarmSpoolStatus::INVALID_FRAME);

  const auto valid = make_wire(source_id, id);
  assert(spool.store(source_id + 1U, id, valid)._status ==
         AlarmSpoolStatus::INVALID_ARGUMENT);
  assert(spool.store(source_id, make_message_id(9U), valid)._status ==
         AlarmSpoolStatus::INVALID_ARGUMENT);
  assert(spool.stats()._stats._files == 0U);
}

/* 统计必须使用 64 位文件长度，不能把大文件截断为 uint16_t。 */
void test_corrupt_byte_count_does_not_truncate() {
  TemporaryDirectory root("spool-wide-stats");
  AlarmSpool spool({root.path(), AlarmSpoolKind::OUTBOX, 0U});
  assert(spool.ready());
  const fs::path large = root.path() / "corrupt" / "large.bad";
  {
    std::ofstream output(large, std::ios::binary);
    const std::vector<char> bytes(70'000U, 'x');
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
  }
  const AlarmSpoolStatsResult stats = spool.stats();
  assert(stats.success());
  assert(stats._stats._corrupt_files == 1U);
  assert(stats._stats._corrupt_bytes == 70'000U);
}

} // namespace

int main() {
  test_outbox_store_read_reopen_and_remove();
  test_duplicate_and_capacity();
  test_inbox_layout_duplicate_and_quarantine();
  test_corrupt_file_is_quarantined_on_read();
  test_rejects_paths_outside_spool_and_symlinks();
  test_restart_quarantines_stale_tmp_file();
  test_secure_permissions();
  test_concurrent_duplicate_store();
  test_rejects_invalid_frames_and_identity_mismatch();
  test_corrupt_byte_count_does_not_truncate();
  std::cout << "M9_ALARM_SPOOL=PASS\n";
  return 0;
}
