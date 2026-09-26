#ifndef TLSSMON_AIO_TYPES_H
#define TLSSMON_AIO_TYPES_H

#include <cstdint>

namespace TLSSMON {

/** @brief AIO 注册项的句柄；ID 为 0 表示无效。 */
struct AioHandle {
    std::uint64_t _id{0};

    /**
     * @brief 判断句柄是否有效。
     * @return ID 非零时返回 true，否则返回 false。
     */
    explicit operator bool() const noexcept
    {
        return _id != 0;
    }
};

} // namespace TLSSMON

#endif // TLSSMON_AIO_TYPES_H
