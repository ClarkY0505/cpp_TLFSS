#ifndef TLSS_COMMON_NET_WRITE_TYPES_H
#define TLSS_COMMON_NET_WRITE_TYPES_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>

namespace TLSS::NET::detail {
inline constexpr std::size_t kAsyncFileChunk = 64 * 1024;
using FileSendCallback = std::function<void(std::error_code, std::uint64_t)>;
using FileReadCompletion = std::function<void(std::error_code, std::string)>;
using FileReadFunction = std::function<void(off_t, std::size_t, FileReadCompletion)>;
}  // namespace TLSS::NET::detail

#endif  // TLSS_COMMON_NET_WRITE_TYPES_H
