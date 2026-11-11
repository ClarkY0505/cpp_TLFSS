#ifndef __INC_FTP_UPLOAD_REGISTRATION_H__
#define __INC_FTP_UPLOAD_REGISTRATION_H__

#include "common/base/mysql_connection.h"
#include "ftp/metadata_types.h"
namespace TLSS::FileStorage {
// 调用者须已持有对象哈希锁，并成功发布正文。
// connection 是调用者借用的连接，调用前不能已有活动事务。
// 本函数负责开启及提交事务，提交结果未知时原样返回 outcome_unknown。
UploadCommitResult commit_upload_transaction(BASE::DbConnection& connection,
                                             const std::string& username,
                                             const std::string& parent_id,
                                             const std::string& directory,
                                             const std::string& file_name, const std::string& hash);
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_UPLOAD_REGISTRATION_H__
