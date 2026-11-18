#ifndef TLSS_FTP_TRANSFER_VALIDATION_H
#define TLSS_FTP_TRANSFER_VALIDATION_H
#include <sys/types.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftp/transfer_logger.h"

namespace TLSS::FileStorage::Transfer {
inline std::string quote(const std::string& value) {
  std::ostringstream out;
  out << std::quoted(value);
  return out.str();
}

inline std::string safe_error(std::string value) {
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 32 || byte == 127) {
      ch = ' ';
    }
  }
  return value;
}

inline void require_end(std::istream& input) {
  input >> std::ws;
  if (!input.eof()) {
    transfer_logger()->error("unexpected trailing arguments");
    throw std::runtime_error("unexpected trailing arguments");
  }
}

inline std::string read_field(std::istream& input) {
  std::string value;
  if (!(input >> std::quoted(value))) {
    transfer_logger()->error("missing arguments");
    throw std::runtime_error("missing argument");
  }
  return value;
}

inline bool valid_name(const std::string& name) {
  if (name.empty() || name.size() > 255 || name == "." || name == "..") {
    return false;
  }

  bool res = std::all_of(name.begin(), name.end(), [](unsigned char ch) {
    return ch >= 32 && ch != 127 && ch != '/' && ch != '\\';
  });

  /* for (unsigned char ch : name) { */
  /* if (ch < 32 || ch == 127 || ch == '/' || ch == '\\') { */
  /* return false; */
  /* } */
  /* } */
  return res;
}

// 只接受规范绝对目录路径；逐分量校验，不执行归一化或查询目录是否存在。
inline bool valid_directory_path(const std::string& path) {
  if (path.empty() || path.front() != '/' || path.size() > 1024) {
    return false;
  }
  if (path == "/") {
    return true;
  }
  if (path.back() == '/') {
    return false;
  }

  std::size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    if (end == std::string::npos) {
      return valid_name(path.substr(start));
    }
    // 重复斜杠会产生空分量，由 valid_name() 拒绝。
    if (!valid_name(path.substr(start, end - start))) {
      return false;
    }
    start = end + 1;
  }
  return true;
}

// 将相对/绝对虚拟目录解析为规范绝对路径，不查询数据库或磁盘。
// . 保持原目录，.. 返回上级并在根目录停止；非法输入返回 nullopt。
inline std::optional<std::string> resolve_directory_path(
    const std::string& current_directory, const std::string& argument) {
  if (!valid_directory_path(current_directory) || argument.empty()) {
    return std::nullopt;
  }
  // 保留已有分隔符规则，只归一化 . 和 ..。
  if (argument.find("//") != std::string::npos ||
      (argument.size() > 1 && argument.back() == '/')) {
    return std::nullopt;
  }

  // 相对路径先带上当前目录的分量；绝对路径直接从根目录开始。
  const std::string path = argument.front() == '/' ? argument
                          : current_directory == "/" ? "/" + argument
                                                     : current_directory + "/" + argument;
  std::vector<std::string> components;
  std::size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto component = path.substr(start, end == std::string::npos ? end : end - start);

    if (component == "..") {
      if (!components.empty()) {
        components.pop_back();
      }
    } else if (component != ".") {
      // 即使后续 .. 会抵消该分量，也必须先拒绝非法名称。
      if (!valid_name(component)) {
        return std::nullopt;
      }
      components.push_back(component);
    }

    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  std::string result;
  for (const auto& component : components) {
    result += "/" + component;
  }
  if (result.empty()) {
    result = "/";
  }
  if (!valid_directory_path(result)) {
    return std::nullopt;
  }
  return result;
}

inline bool valid_hash(const std::string& hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

inline uint64_t parse_size(const std::string& text) {
  uint64_t size = 0;

  const auto result = std::from_chars(text.data(), text.data() + text.size(), size);

  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    transfer_logger()->error("invalid file size");
    throw std::runtime_error("invalid file size");
  }

  return size;
}

}  // namespace TLSS::FileStorage::Transfer
#endif
