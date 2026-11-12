#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftp/object_backfill.h"
#include "ftp/storage_root_lock.h"

namespace {
int write_count = 0;
std::vector<TLSS::BASE::DbValue> written_params;

TLSS::BASE::DbQueryResult query_result{true, {}};
TLSS::BASE::DbQueryResult list_result{true, {}};
std::vector<std::string> looked_up_hashes;
TLSS::BASE::DbExecuteResult write_result{true, 0, 1, 0};
// 新增磁盘登记场景使用可保留的模拟对象表，验证重试不覆盖已登记记录。
bool persistent_catalog = false;
std::map<std::string, std::string> registered_states;
std::vector<std::vector<TLSS::BASE::DbValue>> write_history;
std::string failed_query_hash;
std::string failed_write_hash;
std::string throwing_query_hash;
std::string throwing_write_hash;
std::function<void(const std::string&)> lookup_hook;

class InjectedDatabaseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace

namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr)
    , _connected(false)
    , _transaction_active(false) {}

DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(const std::string& sql,
                                           const std::vector<DbValue>& params) {
  if (sql.find("SELECT DISTINCT object_hash") == 0) {
    return list_result;
  }

  if (sql.find("SELECT state, unreferenced_at") == 0) {
    if (params.size() != 1 || !params[0].has_value()) {
      throw std::runtime_error("invalid object lookup parameters");
    }

    looked_up_hashes.push_back(*params[0]);
    const auto& hash = *params[0];
    if (lookup_hook)
      lookup_hook(hash);
    if (hash == throwing_query_hash)
      throw InjectedDatabaseError("injected object lookup exception");
    if (hash == failed_query_hash)
      return {false, {}};
    if (persistent_catalog) {
      const auto existing = registered_states.find(hash);
      if (existing != registered_states.end())
        return {true, {{existing->second, std::nullopt}}};
    }
    return query_result;
  }

  throw std::runtime_error("unexpected query: " + sql);
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  if (sql.find("INSERT INTO storage_objects ") != 0 || params.size() != 2 ||
      !params[0].has_value() || !params[1].has_value())
    throw std::runtime_error("unexpected object registration write");
  ++write_count;
  written_params = params;
  write_history.push_back(params);
  if (*params[0] == throwing_write_hash)
    throw InjectedDatabaseError("injected object registration exception");
  if (*params[0] == failed_write_hash)
    return {false, 1213, 0, 0};
  if (persistent_catalog && write_result.success && write_result.affected_rows == 1)
    registered_states.emplace(*params[0], *params[1]);
  return write_result;
}

}  // namespace TLSS::BASE
void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_disk_registration(TLSS::FileStorage::ObjectBackfill& backfill,
                            TLSS::FileStorage::ObjectStorageEngine& objects,
                            const std::filesystem::path& root) {
  using namespace TLSS;
  namespace fs = std::filesystem;
  const std::string first_hash(64, '0');
  const std::string second_hash(64, '1');
  const std::string last_hash(64, 'f');

  auto reset_case = [&] {
    // 静态 object_root 缓存同一个目录，因此复用根目录，只清理测试内容。
    // 保留锁文件，避免持锁期间替换其 inode。
    for (const auto& entry : fs::directory_iterator(root)) {
      if (entry.path().filename() != ".tlss.lock")
        fs::remove_all(entry.path());
    }
    persistent_catalog = true;
    registered_states.clear();
    write_history.clear();
    looked_up_hashes.clear();
    written_params.clear();
    failed_query_hash.clear();
    failed_write_hash.clear();
    throwing_query_hash.clear();
    throwing_write_hash.clear();
    lookup_hook = {};
    query_result = {true, {}};
    list_result = {true, {}};
    write_result = {true, 0, 1, 0};
    write_count = 0;
  };
  auto create_file = [&](const std::string& name) {
    std::ofstream output(root / name);
    output << "unverified content";
    output.close();
    check(output.good(), "create disk registration fixture failed");
  };

  reset_case();
  check(backfill.register_disk_objects(), "empty disk scan was reported as failure");
  check(write_count == 0 && looked_up_hashes.empty(), "empty disk scan accessed object records");

  reset_case();
  auto writer = objects.create_temporary(3);
  writer->append("abc", 3);
  const auto published = objects.publish(*writer);
  create_file(first_hash);  // 文件名合法，但内容损坏，登记为 UNAVAILABLE。
  create_file(second_hash);
  registered_states[second_hash] = "DELETING";
  const std::string unavailable_hash(64, '2');
  create_file(unavailable_hash);
  registered_states[unavailable_hash] = "UNAVAILABLE";
  create_file(".tlss-part-Ab12c3");
  fs::create_symlink("absent", root / std::string(64, '3'));
  fs::create_directory(root / std::string(64, '4'));

  check(backfill.register_disk_objects(), "mixed disk registration failed");
  check(registered_states.at(first_hash) == "UNAVAILABLE" &&
            registered_states.at(published.hash) == "LIVE" &&
            registered_states.at(second_hash) == "DELETING" &&
            registered_states.at(unavailable_hash) == "UNAVAILABLE",
        "disk registration assigned or overwrote an incorrect state");
  check(write_count == 2 && registered_states.size() == 4,
        "disk registration wrote existing or non-regular candidates");
  check(looked_up_hashes == std::vector<std::string>{
            first_hash, second_hash, unavailable_hash, published.hash},
        "disk registration did not use the sorted regular-file candidates");
  check(write_history == std::vector<std::vector<BASE::DbValue>>{
            {first_hash, "UNAVAILABLE"}, {published.hash, "LIVE"}},
        "disk registration inserted incorrect object states");
  check(fs::exists(root / first_hash) && fs::exists(root / published.hash) &&
            fs::exists(root / ".tlss-part-Ab12c3"),
        "object registration unexpectedly deleted disk entries");

  // 同一目录再次执行时，所有记录都已存在，不重复写入或覆盖状态。
  write_count = 0;
  write_history.clear();
  check(backfill.register_disk_objects() && write_count == 0,
        "repeated disk registration overwrote existing records");

  reset_case();
  create_file(first_hash);
  create_file(second_hash);
  failed_query_hash = first_hash;
  check(!backfill.register_disk_objects(), "disk lookup failure was ignored");
  check(write_count == 0 && looked_up_hashes == std::vector<std::string>{first_hash},
        "disk registration continued after a failed lookup");

  reset_case();
  create_file(first_hash);
  create_file(second_hash);
  create_file(last_hash);
  failed_write_hash = second_hash;
  check(!backfill.register_disk_objects(), "later disk insert failure was ignored");
  check(looked_up_hashes == std::vector<std::string>{first_hash, second_hash} &&
            write_count == 2 && registered_states.size() == 1 &&
            registered_states.at(first_hash) == "UNAVAILABLE",
        "partial disk registration lost progress or processed later candidates");
  check(fs::exists(root / first_hash) && fs::exists(root / second_hash) &&
            fs::exists(root / last_hash),
        "failed registration removed a disk object");
  failed_write_hash.clear();
  looked_up_hashes.clear();
  write_count = 0;
  check(backfill.register_disk_objects() && write_count == 2 && registered_states.size() == 3,
        "retry did not preserve earlier registration and finish remaining objects");

  for (const bool query_exception : {false, true}) {
    reset_case();
    create_file(first_hash);
    create_file(second_hash);
    if (query_exception)
      throwing_query_hash = second_hash;
    else
      throwing_write_hash = second_hash;
    bool caught = false;
    try {
      backfill.register_disk_objects();
    } catch (const InjectedDatabaseError&) {
      caught = true;
    }
    check(caught && registered_states.size() == 1 &&
              registered_states.at(first_hash) == "UNAVAILABLE",
          "disk registration swallowed a database exception or lost earlier progress");
  }

  // 扫描结果是快照：查询对象表时文件可能已经消失，仍复用 ENOENT 处理。
  reset_case();
  create_file(first_hash);
  lookup_hook = [&](const std::string& hash) { fs::remove(root / hash); };
  check(backfill.register_disk_objects() && registered_states.at(first_hash) == "UNAVAILABLE",
        "disappearing disk candidate was not registered as unavailable");

  // 确定性模拟扫描后被替换为符号链接，验证非 ENOENT 错误继续向上传播。
  reset_case();
  create_file(first_hash);
  lookup_hook = [&](const std::string& hash) {
    fs::remove(root / hash);
    fs::create_symlink("absent", root / hash);
  };
  bool rethrown = false;
  try {
    backfill.register_disk_objects();
  } catch (const std::system_error& error) {
    rethrown = error.code() == std::errc::too_many_symbolic_link_levels;
  }
  check(rethrown && write_count == 0,
        "disk registration swallowed a filesystem error or inserted after it");
  lookup_hook = {};
}

int main() {
  using namespace TLSS;
  using namespace TLSS::FileStorage;

  char pattern[] = "/tmp/tlss-backfill-test-XXXXXX";
  const char* directory = ::mkdtemp(pattern);
  if (!directory) {
    return 1;
  }

  std::unique_ptr<StorageRootLock> root_lock;
  try {
    if (::setenv("TLSS_STORAGE_ROOT", directory, 1) != 0) {
      throw std::runtime_error("failed to configure storage root");
    }
    root_lock = std::make_unique<StorageRootLock>(directory);

    BASE::DbConnection connection;
    ObjectRepository repository(connection);
    ObjectStorageEngine objects;
    ObjectCoordinator coordinator;
    ObjectBackfill backfill(repository, objects, coordinator);

    const std::string hash(64, '0');
    const bool registered = backfill.register_if_absent(hash);

    check(registered, "missing object registration failed");
    check(write_count == 1, "expected exactly one database write");
    check(written_params.size() == 2 && written_params[0] == BASE::DbValue(hash) &&
              written_params[1] == BASE::DbValue("UNAVAILABLE"),
          "unexpected registration parameters");

    // 场景二：查询失败，不能继续登记。
    write_count = 0;
    written_params.clear();
    query_result = {false, {}};

    check(!backfill.register_if_absent(hash), "query failure was reported as success");
    check(write_count == 0, "query failure triggered a database write");

    // 场景三：已有 DELETING 记录，不能覆盖。
    write_count = 0;
    written_params.clear();
    query_result = {true, {{"DELETING", std::nullopt}}};

    check(backfill.register_if_absent(hash), "existing object was not recognized");
    check(write_count == 0, "existing DELETING record was overwritten");

    query_result = {true, {}};
    write_count = 0;
    written_params.clear();

    auto writer = objects.create_temporary(3);
    writer->append("abc", 3);
    const auto object = objects.publish(*writer);

    const bool result = backfill.register_if_absent(object.hash);
    check(result, "valid object registration failed");
    check(write_count == 1, "valid object registration did not write exactly once");
    check(written_params.size() == 2 && written_params[0] == BASE::DbValue(object.hash) &&
              written_params[1] == BASE::DbValue("LIVE"),
          "valid object was not registered as LIVE");

    query_result = {true, {}};
    write_count = 0;
    written_params.clear();

    const auto object_path = std::filesystem::path(directory) / object.hash;

    std::filesystem::remove(object_path);
    std::ofstream(object_path) << "bad";
    const bool corrupt_registered = backfill.register_if_absent(object.hash);
    check(corrupt_registered, "Failed to register the state of the damaged object");
    check(write_count == 1, "valid object registration did not write exactly once");
    check(written_params.size() == 2 && written_params[0] == BASE::DbValue(object.hash) &&
              written_params[1] == BASE::DbValue("UNAVAILABLE"),
          "valid object was not registered as LIVE");

    // 数据库写入失败：返回失败，并保留正文。
    query_result = {true, {}};
    write_count = 0;
    written_params.clear();
    write_result = {};

    const bool failed_registration = backfill.register_if_absent(object.hash);

    check(!failed_registration, "database write failure was reported as success");
    check(write_count == 1, "expected exactly one attempted database write");
    check(std::filesystem::exists(object_path), "registration failure deleted the object");
    query_result = {true, {}};
    write_result = {true, 0, 1, 0};
    write_count = 0;
    written_params.clear();

    const std::string symlink_hash(64, '1');
    const auto symlink_path = std::filesystem::path(directory) / symlink_hash;

    std::filesystem::create_symlink("missing-target", symlink_path);

    bool rethrown = false;
    try {
      backfill.register_if_absent(symlink_hash);
    } catch (const std::system_error& error) {
      rethrown = error.code() == std::errc::too_many_symbolic_link_levels;
    }

    check(rethrown, "unexpected open error was swallowed");
    check(write_count == 0, "unexpected open error triggered registration");

    const std::string second_hash(64, '2');

    list_result = {true, {{hash}, {second_hash}}};
    query_result = {true, {}};
    write_result = {};  // 模拟第一个对象写入失败

    write_count = 0;
    written_params.clear();
    looked_up_hashes.clear();

    // - 整批返回失败，能够发现之前的 break 问题
    check(!backfill.register_referenced_objects(), "batch failure was reported as success");
    check(write_count == 1, "batch did not stop after the first failed write");
    // - 只查询过第一个对象，没有继续处理第二个
    check(looked_up_hashes.size() == 1 && looked_up_hashes[0] == hash,
          "batch processed another object after failure");

    list_result = {true, {{hash}, {second_hash}}};
    query_result = {true, {}};
    write_result = {true, 0, 1, 0};

    write_count = 0;
    written_params.clear();
    looked_up_hashes.clear();

    const bool batch_registered = backfill.register_referenced_objects();
    check(batch_registered, "successful batch was reported as failure");
    check(write_count == 2, "batch did not register both objects");
    check(looked_up_hashes == std::vector<std::string>{hash, second_hash},
          "batch did not process both objects in order");

    // 清单查询失败。
    write_count = 0;
    written_params.clear();
    looked_up_hashes.clear();
    list_result = {false, {}};

    check(!backfill.register_referenced_objects(), "list query failure was reported as success");
    check(write_count == 0 && looked_up_hashes.empty(),
          "failed list query triggered object processing");

    // 清单查询成功，但没有历史对象。
    write_count = 0;
    written_params.clear();
    looked_up_hashes.clear();
    list_result = {true, {}};

    check(backfill.register_referenced_objects(), "empty object list was reported as failure");
    check(write_count == 0 && looked_up_hashes.empty(),
          "empty object list triggered object processing");

    const std::vector<BASE::DbRow> invalid_rows{BASE::DbRow{}, BASE::DbRow{std::nullopt},
                                                BASE::DbRow{"not-a-valid-hash"},
                                                BASE::DbRow{hash, second_hash}};

    for (const auto& row : invalid_rows) {
      list_result = {true, {row}};
      query_result = {true, {}};
      write_result = {true, 0, 1, 0};

      write_count = 0;
      written_params.clear();
      looked_up_hashes.clear();

      check(!backfill.register_referenced_objects(), "invalid referenced row was accepted");

      check(write_count == 0 && looked_up_hashes.empty(),
            "invalid referenced row triggered object processing");
    }

    test_disk_registration(backfill, objects, directory);
    root_lock.reset();
    std::filesystem::remove_all(directory);
    std::cout << "PASS object backfill\n";
    return 0;
  } catch (const std::exception& error) {
    root_lock.reset();
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
