#ifndef TLSSMON_AIO_TYPES_H
#define TLSSMON_AIO_TYPES_H

#include <cstdint>

namespace TLSSMON {

struct AioHandle {
    std::uint64_t _id{0};

    explicit operator bool() const noexcept
    {
        return _id != 0;
    }
};

} // namespace TLSSMON

#endif // TLSSMON_AIO_TYPES_H
