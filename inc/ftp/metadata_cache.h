#ifndef TLSS_FTP_METADATA_CACHE_H
#define TLSS_FTP_METADATA_CACHE_H

#include "common/cache/arc_cache.h"
#include "ftp/context.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace TLSS::FileStorage {

struct MetadataCacheConfig {
  std::size_t list_capacity = 1024;
  std::size_t download_capacity = 4096;
  std::size_t cd_capacity = 4096;

  // 每类缓存的存储分片数；总容量不会乘以分片数。
  std::size_t arc_shards = 16;
  std::chrono::milliseconds list_ttl{5000};
  std::chrono::milliseconds download_ttl{30000};
  std::chrono::milliseconds cd_ttl{60000};

  // 单个目录快照的近似逻辑大小，不包含所有分配器和历史节点开销。
  std::size_t max_list_bytes = 64 * 1024;
};

struct MetadataCacheCounters {
  std::uint64_t requests = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t loads = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t admission_skips = 0;
};

struct MetadataCacheStats {
  MetadataCacheCounters directory_list;
  MetadataCacheCounters download_hash;
  MetadataCacheCounters cd_path;
};

namespace MetadataCacheDetail {
using Clock = std::chrono::steady_clock;

struct AtomicCounters {
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> loads{0};
  std::atomic<std::uint64_t> coalesced{0};
  std::atomic<std::uint64_t> admission_skips{0};

  MetadataCacheCounters snapshot() const {
    // 统计不参与一致性判断；这些计数不是在同一时刻原子读取的。
    return {
        requests.load(std::memory_order_relaxed),  hits.load(std::memory_order_relaxed),
        misses.load(std::memory_order_relaxed),    loads.load(std::memory_order_relaxed),
        coalesced.load(std::memory_order_relaxed), admission_skips.load(std::memory_order_relaxed)};
  }
};

struct DirectoryGate {
  std::shared_mutex mutex;
  std::uint64_t generation = 0;
  bool versioned_cache_enabled = true;

  void advance() noexcept {
    // 版本耗尽后关闭该分片的版本缓存，防止回绕后旧记录再次有效。
    // CD 不依赖版本，因此仍可以正常命中和过期。
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
      versioned_cache_enabled = false;
      return;
    }
    ++generation;
  }
};

inline std::chrono::milliseconds checked_ttl(std::chrono::milliseconds ttl) {
  // 限制有效期以避免计算抖动和截止时间时溢出；零表示关闭对应缓存。
  if (ttl < std::chrono::milliseconds::zero() || ttl > std::chrono::hours(24)) {
    throw std::invalid_argument(
        "The cache validity period must be between zero and twenty-four hours.");
  }
  return ttl;
}

template <typename T>
struct Entry {
  // 缓存内只复制智能指针，不在 ARC 内部锁中复制整个目录列表。
  std::shared_ptr<const T> value;
  Clock::time_point expires_at{};
  std::uint64_t generation = 0;
};

template <typename T>
struct Bank {
  Bank(std::size_t capacity, std::size_t shards, std::chrono::milliseconds lifetime,
       std::size_t byte_limit = std::numeric_limits<std::size_t>::max())
      : enabled(capacity != 0 && lifetime > std::chrono::milliseconds::zero())
      , ttl(checked_ttl(lifetime))
      , max_bytes(byte_limit)
      , arc(enabled ? capacity : 0, shards) {}

  bool enabled;
  std::chrono::milliseconds ttl;
  std::size_t max_bytes;
  CACHE::HashArcCache<std::string, Entry<T>> arc;
  AtomicCounters counters;
};

enum class Probe { miss, stale, hit };
}  // namespace MetadataCacheDetail

class MetadataCache {
 public:
  using FileList = std::vector<FileInfo>;

  MetadataCache()
      : MetadataCache(MetadataCacheConfig{}) {}

  explicit MetadataCache(const MetadataCacheConfig& config)
      : _lists(config.list_capacity, config.arc_shards, config.list_ttl, config.max_list_bytes)
      , _downloads(config.download_capacity, config.arc_shards, config.download_ttl)
      , _cd_paths(config.cd_capacity, config.arc_shards, config.cd_ttl) {}

  MetadataCache(const MetadataCache&) = delete;
  MetadataCache& operator=(const MetadataCache&) = delete;

  template <typename Loader>
  StorageContext<FileList> list(const std::string& username, const std::string& directory,
                                Loader&& loader) {
    const auto namespace_key = make_key(username, directory);
    return read(_lists, namespace_key, namespace_key, true, std::forward<Loader>(loader));
  }

  template <typename Loader>
  StorageContext<std::string> download(const std::string& username, const std::string& directory,
                                       const std::string& filename, Loader&& loader) {
    return read(_downloads, make_key(username, directory, filename), make_key(username, directory),
                true, std::forward<Loader>(loader));
  }

  template <typename Loader>
  StorageContext<std::string> cd(const std::string& username, const std::string& directory,
                                 const std::string& argument, Loader&& loader) {
    // 当前只新增、不删除/移动目录；已有成功路径只检查 TTL，不跟随写入失效。
    return read(_cd_paths, make_key(username, directory, argument), make_key(username, directory),
                false, std::forward<Loader>(loader));
  }

  template <typename Writer>
  bool write(const std::string& username, const std::string& directory, Writer&& writer) {
    auto& gate = gate_for(make_key(username, directory));
    std::unique_lock<std::shared_mutex> lock(gate.mutex);

    struct InvalidateOnExit {
      MetadataCacheDetail::DirectoryGate& gate;
      ~InvalidateOnExit() {
        gate.advance();
      }
    };

    // 先推进版本再释放独占锁。成功、失败和异常都使旧条目失效，兼顾提交
    // 结果不确定的情况；此清理过程不分配内存，不会在提交后额外抛异常。
    InvalidateOnExit invalidate{gate};
    // 回调只能调用数据库方法，不能再次进入同一缓存的读写接口。
    return std::forward<Writer>(writer)();
  }

  MetadataCacheStats stats() const {
    return {_lists.counters.snapshot(), _downloads.counters.snapshot(),
            _cd_paths.counters.snapshot()};
  }

 private:
  static constexpr std::size_t kDirectoryGateCount = 64;

  static void append_field(std::string& key, const std::string& field) {
    key += std::to_string(field.size());
    key.push_back(':');
    key += field;
  }

  static std::string make_key(const std::string& username, const std::string& directory) {
    std::string key;
    key.reserve(username.size() + directory.size() + 32);
    // 长度前缀避免分隔符歧义。用户名必须来自认证结果，目录必须来自规范
    // 会话路径；此处不自行改变大小写，以免改变数据库中的名称含义。
    append_field(key, username);
    append_field(key, directory);
    return key;
  }

  static std::string make_key(const std::string& username, const std::string& directory,
                              const std::string& argument) {
    auto key = make_key(username, directory);
    append_field(key, argument);
    return key;
  }

  MetadataCacheDetail::DirectoryGate& gate_for(const std::string& namespace_key) {
    const auto index = std::hash<std::string>{}(namespace_key) % _gates.size();
    return _gates[index];
  }

  static bool fits(const std::string& value, std::size_t limit) {
    return value.size() <= limit;
  }

  static bool fits(const FileList& files, std::size_t limit) {
    std::size_t used = sizeof(FileList);
    if (used > limit) {
      return false;
    }
    for (const auto& file : files) {
      const std::array<std::size_t, 5> sizes{sizeof(FileInfo), file.file_name.size(),
                                             file.type.size(), file.create_at.size(),
                                             file.update_at.size()};
      for (const auto bytes : sizes) {
        // 先比较再相加，避免累计逻辑大小发生溢出。
        if (bytes > limit - used) {
          return false;
        }
        used += bytes;
      }
    }
    return true;
  }

  static std::chrono::milliseconds jittered_ttl(std::chrono::milliseconds ttl,
                                                const std::string& key) {
    const int percentage = static_cast<int>(std::hash<std::string>{}(key) % 21) - 10;
    // 按键分散到正负百分之十的范围；短于一百毫秒时可能没有整数抖动。
    return ttl + (ttl / 100) * percentage;
  }

  template <typename T>
  static MetadataCacheDetail::Probe probe(MetadataCacheDetail::Bank<T>& bank,
                                          const std::string& key,
                                          const MetadataCacheDetail::DirectoryGate& gate,
                                          bool versioned, std::shared_ptr<const T>& value) {
    using Probe = MetadataCacheDetail::Probe;
    if (versioned && !gate.versioned_cache_enabled) {
      return Probe::miss;
    }
    MetadataCacheDetail::Entry<T> entry;
    if (!bank.arc.get(key, entry)) {
      return Probe::miss;
    }
    if (!entry.value || entry.expires_at <= MetadataCacheDetail::Clock::now() ||
        (versioned && entry.generation != gate.generation)) {
      return Probe::stale;
    }
    value = std::move(entry.value);
    return Probe::hit;
  }

  template <typename T, typename Loader>
  StorageContext<T> read(MetadataCacheDetail::Bank<T>& bank, const std::string& key,
                         const std::string& namespace_key, bool versioned, Loader&& loader) {
    using Probe = MetadataCacheDetail::Probe;
    auto& counters = bank.counters;
    counters.requests.fetch_add(1, std::memory_order_relaxed);
    if (!bank.enabled) {
      counters.misses.fetch_add(1, std::memory_order_relaxed);
      counters.loads.fetch_add(1, std::memory_order_relaxed);
      return std::forward<Loader>(loader)();
    }

    auto& gate = gate_for(namespace_key);
    std::shared_ptr<const T> cached;
    {
      // 命中路径只持共享协调锁；ARC 内部按存储分片短暂加锁。
      std::shared_lock<std::shared_mutex> lock(gate.mutex);
      probe(bank, key, gate, versioned, cached);
    }
    if (cached) {
      counters.hits.fetch_add(1, std::memory_order_relaxed);
      // 保持引擎值返回接口，结果复制在协调锁和 ARC 锁之外进行。
      return {true, *cached};
    }

    counters.misses.fetch_add(1, std::memory_order_relaxed);
    // 不原地升级共享锁；取得独占锁后重新检查，复用等待期间的填充。
    std::unique_lock<std::shared_mutex> lock(gate.mutex);
    const auto second_probe = probe(bank, key, gate, versioned, cached);
    if (second_probe == Probe::hit) {
      counters.coalesced.fetch_add(1, std::memory_order_relaxed);
      lock.unlock();
      return {true, *cached};
    }
    if (second_probe == Probe::stale) {
      bank.arc.erase(key);
    }
    // 真正未命中时不能 erase，可能存在的历史记录要留给 ARC 调整配额。
    counters.loads.fetch_add(1, std::memory_order_relaxed);
    // 独占协调锁覆盖查询和回填，防止写入穿插；这里没有持有 ARC 内部锁。
    auto result = std::forward<Loader>(loader)();
    if (!result.success) {
      // 失败和不存在不缓存，成功的空目录则可以缓存。
      return result;
    }
    if ((versioned && !gate.versioned_cache_enabled) || !fits(result.result, bank.max_bytes)) {
      counters.admission_skips.fetch_add(1, std::memory_order_relaxed);
      return result;
    }

    auto snapshot = std::make_shared<const T>(std::move(result.result));
    MetadataCacheDetail::Entry<T> entry;
    entry.value = snapshot;
    entry.generation = gate.generation;
    entry.expires_at = MetadataCacheDetail::Clock::now() + jittered_ttl(bank.ttl, key);
    bank.arc.put(key, std::move(entry));
    lock.unlock();
    return {true, *snapshot};
  }

  // 固定分片避免为每个目录永久保留锁和版本。碰撞只会多等待或多失效，
  // 不会混淆用户数据。TTL 在访问时检查，没有后台扫描线程。
  std::array<MetadataCacheDetail::DirectoryGate, kDirectoryGateCount> _gates;
  MetadataCacheDetail::Bank<FileList> _lists;
  MetadataCacheDetail::Bank<std::string> _downloads;
  MetadataCacheDetail::Bank<std::string> _cd_paths;
};

}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_METADATA_CACHE_H
