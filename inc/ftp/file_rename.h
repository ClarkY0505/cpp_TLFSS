#ifndef __INC_FTP_FILE_RENAME_H__
#define __INC_FTP_FILE_RENAME_H__

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"

#include <string>

namespace TLSS::FileStorage {
// connection 调用前不能已有活动事务。
// 在同一目录内重命名文件，不覆盖同名文件或目录。
// 更新 file_name 和 path，保留 parent_id、object_hash。
// 本函数管理事务；提交结果未知时返回 outcome_unknown，异常继续向上传播。
RenameFileResult rename_file_transaction(BASE::DbConnection& connection,
                                         const std::string& username, const std::string& directory,
                                         const std::string& old_name, const std::string& new_name);
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_FILE_RENAME_H__
