#ifndef __INC_FTP_FILE_DELETION_H__
#define __INC_FTP_FILE_DELETION_H__

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"

#include <string>

namespace TLSS::FileStorage {

// connection 由调用者借用，调用前不能已有活动事务。
// directory 是当前目录路径，file_name 是其下的单个文件名。
// 本函数负责开启事务、锁定用户、定位文件、删除文件元数据并提交。
// 不删除磁盘对象，不删除 storage_objects 记录；对象回收交给 GC。
// 提交结果未知时返回 outcome_unknown；异常继续向上传播。
DeleteFileResult delete_file_transaction(BASE::DbConnection& connection,
                                         const std::string& username, const std::string& directory,
                                         const std::string& file_name);

}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_FILE_DELETION_H__
