#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftp/object_backfill.h"

namespace {
int write_count = 0;
std::vector<TLSS::BASE::DbValue> written_params;

TLSS::BASE::DbQueryResult query_result{true, {}};
TLSS::BASE::DbQueryResult list_result{true, {}};
std::vector<std::string> looked_up_hashes;
TLSS::BASE::DbExecuteResult write_result{true, 0, 1, 0};

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
    return query_result;
  }

  throw std::runtime_error("unexpected query: " + sql);
}

DbExecuteResult DbConnection::execute_prepared(const std::string&,
                                               const std::vector<DbValue>& params) {
  ++write_count;
  written_params = params;
  return write_result;
}

}  // namespace TLSS::BASE
void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int main() {
  using namespace TLSS;
  using namespace TLSS::FileStorage;

  char pattern[] = "/tmp/tlss-backfill-test-XXXXXX";
  const char* directory = ::mkdtemp(pattern);
  if (!directory) {
    return 1;
  }

  try {
    if (::setenv("TLSS_STORAGE_ROOT", directory, 1) != 0) {
      throw std::runtime_error("failed to configure storage root");
    }

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

    std::filesystem::remove_all(directory);
    std::cout << "PASS object backfill\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
