#ifndef __INC_FTP_FILE_MOVE_H__
#define __INC_FTP_FILE_MOVE_H__

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"

#include <string>

namespace TLSS::FileStorage {

// connection 由调用者借用，调用前不能已有活动事务。
// source_directory 和 target_directory 为同一用户下的规范绝对目录路径。
// file_name 为源目录下的单个文件名；移动时保留名称，不覆盖同名文件或目录。
// 更新 parent_id 和 path，保留文件 ID、file_name 和 object_hash。
// 本函数管理事务，保留三态结果；异常继续向上传播。
MoveFileResult move_file_transaction(BASE::DbConnection& connection, const std::string& username,
                                     const std::string& source_directory,
                                     const std::string& target_directory,
                                     const std::string& file_name);

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_FILE_MOVE_H__
