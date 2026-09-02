#include "monitor_wire_detail.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

using namespace TLSSMON;

namespace {

/*
 * 验证 uint16_t 按大端序写入和读取。
 *
 * 写入位置故意从 buffer + 1 开始，用于检查非对齐地址。
 */
void test_u16_big_endian_and_unaligned_access() {
  std::array<std::uint8_t, 4> buffer{0xaa, 0xaa, 0xaa, 0xaa};

  Wire::Detail::put_u16(buffer.data() + 1, 0x1234U);

  assert(buffer[0] == 0xaa);
  assert(buffer[1] == 0x12);
  assert(buffer[2] == 0x34);
  assert(buffer[3] == 0xaa);

  const std::uint16_t value = Wire::Detail::get_u16(buffer.data() + 1);

  assert(value == 0x1234U);
}

/*
 * 验证 uint32_t：
 *
 * - 大端序；
 * - 非对齐地址；
 * - 不覆盖前后哨兵字节。
 */
void test_u32_big_endian_and_unaligned_access() {
  std::array<std::uint8_t, 6> buffer{0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};

  Wire::Detail::put_u32(buffer.data() + 1, 0x12345678U);

  assert(buffer[0] == 0xaa);
  assert(buffer[1] == 0x12);
  assert(buffer[2] == 0x34);
  assert(buffer[3] == 0x56);
  assert(buffer[4] == 0x78);
  assert(buffer[5] == 0xaa);

  const std::uint32_t value = Wire::Detail::get_u32(buffer.data() + 1);

  assert(value == 0x12345678U);
}

/*
 * 验证 uint64_t 大端序。
 */
void test_u64_big_endian_and_unaligned_access() {
  std::array<std::uint8_t, 10> buffer{0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
                                      0xaa, 0xaa, 0xaa, 0xaa, 0xaa};

  Wire::Detail::put_u64(buffer.data() + 1, UINT64_C(0x0102030405060708));

  assert(buffer[0] == 0xaa);
  assert(buffer[1] == 0x01);
  assert(buffer[2] == 0x02);
  assert(buffer[3] == 0x03);
  assert(buffer[4] == 0x04);
  assert(buffer[5] == 0x05);
  assert(buffer[6] == 0x06);
  assert(buffer[7] == 0x07);
  assert(buffer[8] == 0x08);
  assert(buffer[9] == 0xaa);

  const std::uint64_t value = Wire::Detail::get_u64(buffer.data() + 1);

  assert(value == UINT64_C(0x0102030405060708));
}

/*
 * 验证无符号整数的最小值和最大值。
 */
void test_unsigned_boundaries_round_trip() {
  std::array<std::uint8_t, 8> buffer{};

  Wire::Detail::put_u16(buffer.data(),
                        std::numeric_limits<std::uint16_t>::min());

  assert(Wire::Detail::get_u16(buffer.data()) ==
         std::numeric_limits<std::uint16_t>::min());

  Wire::Detail::put_u16(buffer.data(),
                        std::numeric_limits<std::uint16_t>::max());

  assert(Wire::Detail::get_u16(buffer.data()) ==
         std::numeric_limits<std::uint16_t>::max());

  Wire::Detail::put_u32(buffer.data(),
                        std::numeric_limits<std::uint32_t>::min());

  assert(Wire::Detail::get_u32(buffer.data()) ==
         std::numeric_limits<std::uint32_t>::min());

  Wire::Detail::put_u32(buffer.data(),
                        std::numeric_limits<std::uint32_t>::max());

  assert(Wire::Detail::get_u32(buffer.data()) ==
         std::numeric_limits<std::uint32_t>::max());

  Wire::Detail::put_u64(buffer.data(),
                        std::numeric_limits<std::uint64_t>::min());

  assert(Wire::Detail::get_u64(buffer.data()) ==
         std::numeric_limits<std::uint64_t>::min());

  Wire::Detail::put_u64(buffer.data(),
                        std::numeric_limits<std::uint64_t>::max());

  assert(Wire::Detail::get_u64(buffer.data()) ==
         std::numeric_limits<std::uint64_t>::max());
}

/*
 * 验证有符号 seconds 按 64 位补码往返。
 */
void test_signed_seconds_round_trip() {
  constexpr std::array<std::int64_t, 7> values{
      std::numeric_limits<std::int64_t>::min(),
      INT64_C(-72623859790382856),
      INT64_C(-1),
      INT64_C(0),
      INT64_C(1),
      INT64_C(72623859790382856),
      std::numeric_limits<std::int64_t>::max()};

  std::array<std::uint8_t, 8> buffer{};

  for (const std::int64_t expected : values) {
    Wire::Detail::put_i64_twos_complement(buffer.data(), expected);

    const std::int64_t actual =
        Wire::Detail::get_i64_twos_complement(buffer.data());

    assert(actual == expected);
  }
}

/*
 * -1 的 64 位补码必须全部是 0xff。
 */
void test_negative_one_twos_complement_bytes() {
  std::array<std::uint8_t, 8> buffer{};

  Wire::Detail::put_i64_twos_complement(buffer.data(), INT64_C(-1));

  for (const std::uint8_t byte : buffer) {
    assert(byte == 0xff);
  }

  assert(Wire::Detail::get_i64_twos_complement(buffer.data()) == INT64_C(-1));
}

/*
 * INT64_MIN 的补码必须为：
 *
 * 80 00 00 00 00 00 00 00
 */
void test_int64_min_twos_complement_bytes() {
  std::array<std::uint8_t, 8> buffer{};

  Wire::Detail::put_i64_twos_complement(
      buffer.data(), std::numeric_limits<std::int64_t>::min());

  assert(buffer[0] == 0x80);

  for (std::size_t index = 1; index < buffer.size(); ++index) {
    assert(buffer[index] == 0x00);
  }

  assert(Wire::Detail::get_i64_twos_complement(buffer.data()) ==
         std::numeric_limits<std::int64_t>::min());
}

/*
 * 纳秒合法范围：
 *
 * 0 <= nanoseconds < 1,000,000,000
 */
void test_nanoseconds_range() {
  assert(Wire::Detail::is_valid_nanoseconds(0U));

  assert(Wire::Detail::is_valid_nanoseconds(999'999'999U));

  assert(!Wire::Detail::is_valid_nanoseconds(1'000'000'000U));

  assert(!Wire::Detail::is_valid_nanoseconds(
      std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

int main() {
  test_u16_big_endian_and_unaligned_access();
  test_u32_big_endian_and_unaligned_access();
  test_u64_big_endian_and_unaligned_access();
  test_unsigned_boundaries_round_trip();
  test_signed_seconds_round_trip();
  test_negative_one_twos_complement_bytes();
  test_int64_min_twos_complement_bytes();
  test_nanoseconds_range();
  return 0;
}
