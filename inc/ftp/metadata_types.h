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

}  // namespace TLSS::FileStorage
#endif
