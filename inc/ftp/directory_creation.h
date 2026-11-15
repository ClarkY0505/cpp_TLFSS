#ifndef __INC_FTP_DIRECTORY_CREATION_H__
#define __INC_FTP_DIRECTORY_CREATION_H__

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"

#include <string>

namespace TLSS::FileStorage {

// connection 由调用者提供，调用前不能已有活动事务
// current_directory 是父目录路径，directory_name 是新建子目录的单个名称
// 本函数负责开启事务，先锁用户，再查询父目录、插入目录并提交
// 拒绝覆盖同名文件或目录
// 提交结果未知时返回 outcome_unknown；异常继续向上传播
CreateDirectoryResult create_directory_transaction(BASE::DbConnection& connection,
                                                   const std::string& username,
                                                   const std::string& current_directory,
                                                   const std::string& directory_name);

}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_DIRECTORY_CREATION_H
