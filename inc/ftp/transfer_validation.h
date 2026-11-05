#ifndef TLSS_FTP_TRANSFER_VALIDATION_H
#define TLSS_FTP_TRANSFER_VALIDATION_H
#include <sys/types.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

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
