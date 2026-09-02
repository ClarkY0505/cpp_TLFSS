#include "udp_publisher.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

/*
 * 测试辅助对象：在 127.0.0.1 上创建一个使用系统临时端口的 UDP
 * 接收端，并通过 RAII 在案例结束时关闭 socket。
 *
 * 每个测试案例使用独立接收端，防止上一个案例遗留的数据报影响
 * 下一个案例的断言。
 */
class LoopbackUdpReceiver final {
public:
  LoopbackUdpReceiver() {
    _socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(_socket >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int bind_result =
        ::bind(_socket,
               reinterpret_cast<const sockaddr *>(&address),
               static_cast<socklen_t>(sizeof(address)));
    assert(bind_result == 0);

    socklen_t address_size =
        static_cast<socklen_t>(sizeof(address));
    const int name_result =
        ::getsockname(_socket,
                      reinterpret_cast<sockaddr *>(&address),
                      &address_size);
    assert(name_result == 0);
    assert(address_size == sizeof(address));

    _port = ntohs(address.sin_port);
    assert(_port != 0U);
  }

  ~LoopbackUdpReceiver() {
    if (_socket >= 0) {
      ::close(_socket);
      _socket = -1;
    }
  }

  LoopbackUdpReceiver(const LoopbackUdpReceiver &) = delete;
  LoopbackUdpReceiver &operator=(const LoopbackUdpReceiver &) = delete;
  LoopbackUdpReceiver(LoopbackUdpReceiver &&) = delete;
  LoopbackUdpReceiver &operator=(LoopbackUdpReceiver &&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept {
    return _port;
  }

  [[nodiscard]] bool has_datagram(int timeout_ms) const {
    pollfd descriptor{};
    descriptor.fd = _socket;
    descriptor.events = POLLIN;

    const int poll_result = ::poll(&descriptor, 1U, timeout_ms);
    assert(poll_result >= 0);

    if (poll_result == 0) {
      return false;
    }

    assert((descriptor.revents & POLLIN) != 0);
    return true;
  }

  [[nodiscard]] std::vector<std::uint8_t> receive() const {
    assert(has_datagram(1'000));

    std::array<std::uint8_t, Wire::V2_MAX_DATAGRAM_SIZE + 1U> buffer{};
    const ssize_t received =
        ::recvfrom(_socket,
                   buffer.data(),
                   buffer.size(),
                   0,
                   nullptr,
                   nullptr);
    assert(received >= 0);

    return std::vector<std::uint8_t>(
        buffer.begin(),
        buffer.begin() + received);
  }

private:
  mutable int _socket{-1};
  std::uint16_t _port{0};
};

MonData::StoredRecord make_numeric_record(
    std::uint32_t value = 123U,
    std::uint32_t state = 2U,
    std::string description = "cpu-count") {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{
              0x11223344U,
              0x55667788U,
              0x99aabbccU,
              0xddeeff00U},
          std::move(description),
          MonData::NumericValue{value, state}},
      MonData::MonitorTimestamp{
          std::chrono::seconds{1'700'000'000} +
          std::chrono::nanoseconds{123'456'789}}};
}

MonData::StoredRecord make_string_record(
    std::string value,
    std::string description = "service-status") {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{11U, 22U, 33U, 44U},
          std::move(description),
          std::move(value)},
      MonData::MonitorTimestamp{
          std::chrono::seconds{1'700'000'001} +
          std::chrono::nanoseconds{987'654'321}}};
}

/*
 * 公共断言：一次 send() 必须产生且只产生一个 UDP 数据报，并且接收端
 * 看到的字节必须与指定版本 Codec 的输出完全相同。
 */
void assert_single_datagram_equals(
    Wire::UdpPublisher &publisher,
    LoopbackUdpReceiver &receiver,
    const MonData::StoredRecord &record,
    Wire::WireVersion expected_version) {
  const Wire::EncodeResult expected =
      Wire::encode(record, expected_version);
  assert(expected._status == Wire::WireStatus::SUCCESS);

  const Wire::UdpPublishResult sent = publisher.send(record);
  assert(sent._status == Wire::UdpPublishStatus::SUCCESS);
  assert(sent._wire_status == Wire::WireStatus::SUCCESS);
  assert(sent._bytes_sent == expected._bytes.size());
  assert(sent._system_error == 0);

  const std::vector<std::uint8_t> received = receiver.receive();
  assert(received == expected._bytes);
  assert(!received.empty());
  assert(received.front() ==
         static_cast<std::uint8_t>(expected_version));

  /*
   * 再等待一小段时间，确认一次 send() 没有拆成两个数据报或重复发送。
   */
  assert(!receiver.has_datagram(50));
}

/*
 * 测试功能：固定阶段 8 的公共 C++ 接口契约。
 *
 * 测试步骤：
 * 1. 编译期确认配置不能默认构造，调用者必须显式提供版本。
 * 2. 编译期确认 Publisher 不可复制、不可移动。
 * 3. 固定 send() 的 const 成员函数签名和返回类型。
 */
using ExpectedSend = Wire::UdpPublishResult (
    Wire::UdpPublisher::*)(const MonData::StoredRecord &) const;

static_assert(!std::is_default_constructible_v<
                  Wire::UdpPublisherConfig>,
              "UdpPublisherConfig must require an explicit version");
static_assert(std::is_constructible_v<
                  Wire::UdpPublisherConfig,
                  Wire::UdpEndpoint,
                  Wire::WireVersion>,
              "UdpPublisherConfig must accept endpoint and version");
static_assert(!std::is_copy_constructible_v<Wire::UdpPublisher>,
              "UdpPublisher must not duplicate its socket");
static_assert(!std::is_copy_assignable_v<Wire::UdpPublisher>,
              "UdpPublisher must not duplicate its socket");
static_assert(!std::is_move_constructible_v<Wire::UdpPublisher>,
              "UdpPublisher lifetime must remain stable");
static_assert(!std::is_move_assignable_v<Wire::UdpPublisher>,
              "UdpPublisher configuration must remain stable");
static_assert(std::is_same_v<decltype(&Wire::UdpPublisher::send),
                             ExpectedSend>,
              "UdpPublisher::send signature changed");

/*
 * 测试功能：V1 配置被 Publisher 按值保存，构造完成后可以查询到相同的
 * Endpoint 和明确选择的版本。
 *
 * 测试步骤：
 * 1. 使用 loopback 临时端口和 V1 构造 Publisher。
 * 2. 验证 socket 初始化成功。
 * 3. 验证 address、port 和 version 与构造配置完全一致。
 */
void test_v1_configuration_is_explicit_and_stable() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V1});

  assert(publisher.ready());
  assert(publisher.setup_status() ==
         Wire::UdpPublishStatus::SUCCESS);
  assert(publisher.version() == Wire::WireVersion::V1);
  assert(publisher.config()._version == Wire::WireVersion::V1);
  assert(publisher.config()._endpoint._address == "127.0.0.1");
  assert(publisher.config()._endpoint._port == receiver.port());
}

/*
 * 测试功能：V2 同样必须由调用者显式选择，不能由记录类型或内容自动
 * 猜测协议版本。
 *
 * 测试步骤：
 * 1. 使用相同形式的 Endpoint，但明确传入 V2。
 * 2. 验证 Publisher 初始化成功且只报告 V2。
 */
void test_v2_configuration_is_explicit_and_stable() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V2});

  assert(publisher.ready());
  assert(publisher.setup_status() ==
         Wire::UdpPublishStatus::SUCCESS);
  assert(publisher.version() == Wire::WireVersion::V2);
  assert(publisher.config()._version == Wire::WireVersion::V2);
}

/*
 * 测试功能：空地址、零端口和非法 IPv4 地址必须在初始化阶段被拒绝，
 * 不能拖到 sendto() 时才暴露。
 *
 * 测试步骤：分别构造三种非法 Endpoint，验证 ready()==false，且状态都
 * 为 INVALID_ENDPOINT。
 */
void test_invalid_endpoints_are_rejected() {
  Wire::UdpPublisher empty_address({
      {"", 9100U},
      Wire::WireVersion::V1});
  Wire::UdpPublisher zero_port({
      {"127.0.0.1", 0U},
      Wire::WireVersion::V1});
  Wire::UdpPublisher malformed_address({
      {"not-an-ipv4-address", 9100U},
      Wire::WireVersion::V2});

  assert(!empty_address.ready());
  assert(empty_address.setup_status() ==
         Wire::UdpPublishStatus::INVALID_ENDPOINT);
  assert(!zero_port.ready());
  assert(zero_port.setup_status() ==
         Wire::UdpPublishStatus::INVALID_ENDPOINT);
  assert(!malformed_address.ready());
  assert(malformed_address.setup_status() ==
         Wire::UdpPublishStatus::INVALID_ENDPOINT);
}

/*
 * 测试功能：即使调用者通过强制类型转换构造出未知枚举值，Publisher
 * 也必须拒绝它，不能默认回退到 V1 或 V2。
 *
 * 测试步骤：
 * 1. 以版本 99 构造 Publisher。
 * 2. 验证初始化状态为 INVALID_VERSION。
 * 3. 调用 send()，验证仍返回 INVALID_VERSION 且没有发送字节。
 */
void test_unknown_version_is_rejected() {
  Wire::UdpPublisher publisher({
      {"127.0.0.1", 9100U},
      static_cast<Wire::WireVersion>(99U)});

  assert(!publisher.ready());
  assert(publisher.setup_status() ==
         Wire::UdpPublishStatus::INVALID_VERSION);

  const Wire::UdpPublishResult result =
      publisher.send(make_numeric_record());
  assert(result._status ==
         Wire::UdpPublishStatus::INVALID_VERSION);
  assert(result._bytes_sent == 0U);
}

/*
 * 测试功能：V1 数值记录必须使用 V1 Codec，并作为一个固定 26 字节的
 * UDP 数据报发送。
 *
 * 测试步骤：
 * 1. 创建 V1 Publisher 和数值 StoredRecord。
 * 2. 调用一次 send()。
 * 3. 验证接收字节等于 encode(record, V1)，长度为 26，且没有第二包。
 */
void test_v1_numeric_is_sent_as_one_26_byte_datagram() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V1});
  const MonData::StoredRecord record = make_numeric_record();

  assert_single_datagram_equals(
      publisher,
      receiver,
      record,
      Wire::WireVersion::V1);
  assert(Wire::encode(record, Wire::WireVersion::V1)._bytes.size() ==
         Wire::V1_NUMERIC_DATAGRAM_SIZE);
}

/*
 * 测试功能：V1 最大合法字符串仍然能够通过 Publisher 发送完整的
 * 82 字节兼容数据报。
 *
 * 测试步骤：
 * 1. 构造恰好 63 字节的字符串记录。
 * 2. 使用 V1 Publisher 发送。
 * 3. 验证接收结果与 V1 Codec 一致、总长为 82，且只收到一个数据报。
 */
void test_v1_max_string_is_sent_as_one_82_byte_datagram() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V1});
  const MonData::StoredRecord record =
      make_string_record(std::string(63U, 'V'));

  assert_single_datagram_equals(
      publisher,
      receiver,
      record,
      Wire::WireVersion::V1);
  assert(Wire::encode(record, Wire::WireVersion::V1)._bytes.size() ==
         Wire::V1_MAX_DATAGRAM_SIZE);
}

/*
 * 测试功能：V2 数值发送必须保留完整 Key、description、数值 state 和
 * timestamp，而不是退化为 V1 数据布局。
 *
 * 测试步骤：
 * 1. 使用 V2 Publisher 发送带完整元数据的数值记录。
 * 2. 验证数据报字节与 V2 Codec 完全一致且只发送一次。
 * 3. 解码实际收到的数据报，逐项比较元数据和值。
 */
void test_v2_numeric_preserves_complete_record() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V2});
  const MonData::StoredRecord record = make_numeric_record();

  const Wire::UdpPublishResult sent = publisher.send(record);
  assert(sent.success());

  const std::vector<std::uint8_t> received = receiver.receive();
  const Wire::EncodeResult expected =
      Wire::encode(record, Wire::WireVersion::V2);
  assert(received == expected._bytes);
  assert(!receiver.has_datagram(50));

  const Wire::DecodeResult decoded =
      Wire::decode(received.data(), received.size());
  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V2);
  assert(decoded._record->_data._key == record._data._key);
  assert(decoded._record->_data._description ==
         record._data._description);
  assert(decoded._record->_data._value == record._data._value);
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == record._changed_at);
}

/*
 * 测试功能：V2 Publisher 必须完整发送 UTF-8 和内嵌 NUL，不能采用
 * V1 的 NUL 终止字符串语义。
 *
 * 测试步骤：
 * 1. 构造“中文 + NUL + ASCII”的字符串。
 * 2. 使用 V2 Publisher 发送并解码实际收到的数据报。
 * 3. 验证字符串长度、NUL 位置和全部字节均保持不变。
 */
void test_v2_string_preserves_utf8_and_embedded_nul() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V2});

  std::string value = u8"运行";
  const std::size_t nul_position = value.size();
  value.push_back('\0');
  value += "ok";
  const MonData::StoredRecord record =
      make_string_record(value, u8"服务状态");

  const Wire::UdpPublishResult sent = publisher.send(record);
  assert(sent.success());

  const std::vector<std::uint8_t> received = receiver.receive();
  assert(!receiver.has_datagram(50));

  const Wire::DecodeResult decoded =
      Wire::decode(received.data(), received.size());
  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());

  const auto *decoded_value =
      std::get_if<std::string>(&decoded._record->_data._value);
  assert(decoded_value != nullptr);
  assert(*decoded_value == value);
  assert(decoded_value->size() == value.size());
  assert((*decoded_value)[nul_position] == '\0');
  assert(decoded._record->_data._description == u8"服务状态");
}

/*
 * 测试功能：同一个 Publisher 连续发送不同类型记录时，协议版本必须
 * 始终保持构造时选择的 V1，不能根据记录内容在运行期间切换。
 *
 * 测试步骤：
 * 1. 用同一个 V1 Publisher 先发送数值，再发送字符串。
 * 2. 分别接收两个数据报。
 * 3. 验证两个数据报第 0 字节都为 1，并分别等于 V1 编码结果。
 */
void test_one_publisher_never_changes_version() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V1});
  const MonData::StoredRecord numeric = make_numeric_record();
  const MonData::StoredRecord text = make_string_record("ready");

  assert(publisher.send(numeric).success());
  const std::vector<std::uint8_t> numeric_datagram = receiver.receive();

  assert(publisher.send(text).success());
  const std::vector<std::uint8_t> string_datagram = receiver.receive();

  assert(numeric_datagram.front() ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));
  assert(string_datagram.front() ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));
  assert(numeric_datagram ==
         Wire::encode(numeric, Wire::WireVersion::V1)._bytes);
  assert(string_datagram ==
         Wire::encode(text, Wire::WireVersion::V1)._bytes);
  assert(publisher.version() == Wire::WireVersion::V1);
  assert(!receiver.has_datagram(50));
}

/*
 * 测试功能：V1 编码失败时 Publisher 必须直接返回错误，绝不能发送
 * 截断字符串或空数据报。
 *
 * 测试步骤：
 * 1. 构造超过 V1 63 字节上限的字符串。
 * 2. 调用 V1 Publisher::send()。
 * 3. 验证返回 ENCODE_ERROR/STRING_TOO_LONG、发送字节为 0，接收端无包。
 */
void test_v1_encode_failure_does_not_send_datagram() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V1});
  const MonData::StoredRecord record =
      make_string_record(std::string(64U, 'X'));

  const Wire::UdpPublishResult result = publisher.send(record);

  assert(result._status == Wire::UdpPublishStatus::ENCODE_ERROR);
  assert(result._wire_status == Wire::WireStatus::STRING_TOO_LONG);
  assert(result._bytes_sent == 0U);
  assert(result._system_error == 0);
  assert(!receiver.has_datagram(100));
}

/*
 * 测试功能：V2 的 1200 字节边界必须可以作为一个完整 UDP 数据报
 * 发送，Publisher 不能自行拆包。
 *
 * 测试步骤：
 * 1. 构造 description=100 字节、value=1060 字节的字符串记录。
 * 2. 连同 40 字节头部，编码结果恰好为 1200 字节。
 * 3. 发送后验证接收端一次得到全部 1200 字节且没有第二包。
 */
void test_v2_exact_1200_byte_datagram_is_sent_once() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V2});
  const MonData::StoredRecord record =
      make_string_record(
          std::string(1'060U, 'B'),
          std::string(100U, 'A'));

  const Wire::EncodeResult encoded =
      Wire::encode(record, Wire::WireVersion::V2);
  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V2_MAX_DATAGRAM_SIZE);

  assert_single_datagram_equals(
      publisher,
      receiver,
      record,
      Wire::WireVersion::V2);
}

/*
 * 测试功能：超过 V2 1200 字节上限时必须停在编码阶段，不能让内核
 * 尝试发送超出协议契约的数据报。
 *
 * 测试步骤：
 * 1. 构造总长度为 1201 字节的 V2 字符串记录。
 * 2. 验证 send() 返回 ENCODE_ERROR/DATAGRAM_TOO_LARGE。
 * 3. 验证接收端没有收到任何数据报。
 */
void test_v2_oversized_record_does_not_send_datagram() {
  LoopbackUdpReceiver receiver;
  Wire::UdpPublisher publisher({
      {"127.0.0.1", receiver.port()},
      Wire::WireVersion::V2});
  const MonData::StoredRecord record =
      make_string_record(std::string(1'161U, 'Z'), "");

  const Wire::UdpPublishResult result = publisher.send(record);

  assert(result._status == Wire::UdpPublishStatus::ENCODE_ERROR);
  assert(result._wire_status == Wire::WireStatus::DATAGRAM_TOO_LARGE);
  assert(result._bytes_sent == 0U);
  assert(result._system_error == 0);
  assert(!receiver.has_datagram(100));
}

} // namespace

int main() {
  test_v1_configuration_is_explicit_and_stable();
  test_v2_configuration_is_explicit_and_stable();
  test_invalid_endpoints_are_rejected();
  test_unknown_version_is_rejected();
  test_v1_numeric_is_sent_as_one_26_byte_datagram();
  test_v1_max_string_is_sent_as_one_82_byte_datagram();
  test_v2_numeric_preserves_complete_record();
  test_v2_string_preserves_utf8_and_embedded_nul();
  test_one_publisher_never_changes_version();
  test_v1_encode_failure_does_not_send_datagram();
  test_v2_exact_1200_byte_datagram_is_sent_once();
  test_v2_oversized_record_does_not_send_datagram();

  std::cout << "UDP_PUBLISHER_EXPLICIT_VERSION=PASS\n";
  std::cout << "UDP_PUBLISHER_V1=PASS\n";
  std::cout << "UDP_PUBLISHER_V2=PASS\n";
  std::cout << "UDP_PUBLISHER_SINGLE_DATAGRAM=PASS\n";
  std::cout << "UDP_PUBLISHER_ENCODE_FAILURE=PASS\n";
  return 0;
}
