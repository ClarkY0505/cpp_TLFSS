#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include "ftp/object_backfill.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {
ObjectBackfill::ObjectBackfill(ObjectRepository& repository, ObjectStorageEngine& objects,
                               ObjectCoordinator& coordinator)
    : _repository(repository)
    , _objects(objects)
    , _coordinator(coordinator) {}

bool ObjectBackfill::register_if_absent(const std::string& hash) {
  // 1. 获取对应哈希锁。
  std::lock_guard<std::mutex> lock(_coordinator.mutex_for(hash));

  // 2. 调用 find_object()：查询失败返回 false；已有记录则保持原样，返回 true。
  BASE::DbQueryResult qu_res = _repository.find_object(hash);
  // qu_res.success中true状态下有两种情况
  // - qu_res.rows 为空，查询执行成功，但没有该哈希的记录
  // - qu_res.rows 非空，找到了该哈希的对象记录，说明已登记
  if (!qu_res.success) {
    return false;
  }
  if (!qu_res.rows.empty()) {
    return true;
  }

  // 3. 没有记录时，调用 verify_object()：
  /* bool verified = _objects.verify_object(hash); */
  // - 返回 true：准备登记 "LIVE"。
  /* if (verified) { */
  /* ex_res = _repository.insert_object(hash, "LIVE"); */
  /* } */
  // - 返回 false：准备登记 "UNAVAILABLE"。
  /* else { */
  /* ex_res = _repository.insert_object(hash, "UNAVAILABLE"); */
  /* } */

  // 3. 没有记录时，调用 verify_object()：
  // - 返回 true：准备登记 "LIVE"。
  // - 返回 false：准备登记 "UNAVAILABLE"。
  // - 捕获 std::system_error，仅文件不存在时登记 "UNAVAILABLE"；其他错误重新抛出。
  std::string state;
  try {
    const bool verified = _objects.verify_object(hash);
    state = verified ? "LIVE" : "UNAVAILABLE";
  } catch (const std::system_error& error) {
    if (error.code() != std::errc::no_such_file_or_directory) {
      throw;
    }

    state = "UNAVAILABLE";
  }

  // 4. 调用 insert_object()，返回其 success。
  BASE::DbExecuteResult ex_res = _repository.insert_object(hash, state);
  return ex_res.success;
}

bool ObjectBackfill::register_referenced_objects() {
  // 1. 调用 _repository.list_referenced_hashes()。
  BASE::DbQueryResult qu_res;
  qu_res = _repository.list_referenced_hashes();
  // 2. 查询失败，返回 false。
  if (!qu_res.success) {
    return false;
  }
  // 3. 遍历结果，每一行都应当：
  for (const auto& row : qu_res.rows) {
    // 恰好有一列。
    if (row.size() != 1 || !row[0].has_value()) {
      return false;
    }
    // 取出这一列保存的字符串。
    const std::string& hash = *row[0];

    // 检查是否为合法的 SHA-256 字符串。
    if (!Transfer::valid_hash(hash)) {
      return false;
    }
    // 4. 对每个哈希调用已有的 register_if_absent()；返回 false 就立即停止。
    if (!register_if_absent(hash)) {
      return false;
    }
  }
  // 5. 全部处理成功后返回 true，空清单也返回 true。
  return true;
}

bool ObjectBackfill::register_disk_objects() {
  std::vector<std::string> hashes = _objects.list_object_hashes();
  for (auto& hash : hashes) {
    if (!register_if_absent(hash)) {
      return false;
    }
  }

  return true;
}

}  // namespace TLSS::FileStorage
