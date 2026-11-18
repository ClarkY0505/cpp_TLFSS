#ifndef TLSS_FTP_METADATA_TYPES_H
#define TLSS_FTP_METADATA_TYPES_H
#include <string>
namespace TLSS::FileStorage {
template <typename T>
struct StorageContext {
  bool success = false;
  T result{};
};

struct FileInfo {
  std::string file_name;
  std::string type;
  std::string create_at;
  std::string update_at;
};

enum class UploadCommitResult { committed, failed, outcome_unknown };

enum class DeleteFileResult {
  committed,       // 已确认事务提交，文件元数据已删除
  failed,          // 未确认成功；本次未提交，不能报告删除成功
  outcome_unknown  // 提交结果未知，不能假定已经回滚
};

enum class DeleteDirectoryResult {
  committed,       // 空目录删除已提交
  not_empty,       // 存在文件或子目录，拒绝删除
  failed,          // 参数错误、目标不存在、根目录保护或数据库操作失败
  outcome_unknown  // 提交结果未知
};

enum class CreateDirectoryResult {
  committed,       // 已确认提交，目录创建成功
  failed,          // 参数、查询或写入失败，本次未提交
  outcome_unknown  // 提交结果未知，不能假定目录没有创建
};

enum class DeleteDirectoryTreeResult {
  committed,       // 已确认整棵目录树的元数据删除事务提交成功
  failed,          // 参数校验、查询或删除等操作失败
  outcome_unknown  // 提交结果未知，不能认定已经回滚
};

enum class RenameFileResult {
  committed,       // 已确认文件重命名事务提交成功
  failed,          // 参数错误、源文件不存在、目标重名或数据库操作失败
  outcome_unknown  // 提交结果未知，不能认定已经回滚
};

enum class MoveFileResult {
  committed,  // 已确认文件移动事务提交成功
  failed,     // 参数错误、源文件或目录不存在、目标重名或数据库操作失败
  outcome_unknown  // 提交结果未知，不能认定已经回滚
};

}  // namespace TLSS::FileStorage
#endif
