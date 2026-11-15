#ifndef __INC_FTP_DIRECTORY_DELETION_H__
#define __INC_FTP_DIRECTORY_DELETION_H__

#include <string>

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"

namespace TLSS::FileStorage {

// connection 由调用者提供，调用前不能已有活动事务
// current_directory：当前目录路径
// directory_name：当前目录下待删除的单个子目录名
// 本函数负责开启事务、锁定用户、定位目录、检查为空后删除并提交
// 拒绝删除用户根目录；发现任意直接子项时返回 not_empty
// 只操作数据库，提交结果未知时返回 outcome_unknown；异常继续向上传播
DeleteDirectoryResult delete_empty_directory_transaction(BASE::DbConnection& connection,
                                                         const std::string& username,
                                                         const std::string& current_directory,
                                                         const std::string& directory_name);

// 调用前 connection 不能已有活动事务。
// 删除当前目录下指定子目录及其全部后代，拒绝删除用户根目录。
// 整次操作共用一个事务，全部删除完成后统一提交。
// 仅删除元数据，磁盘对象交给 GC；异常继续向上传播。
DeleteDirectoryTreeResult delete_directory_tree_transaction(BASE::DbConnection& connection,
                                                            const std::string& username,
                                                            const std::string& current_directory,
                                                            const std::string& directory_name);
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_DIRECTORY_DELETION_H__
