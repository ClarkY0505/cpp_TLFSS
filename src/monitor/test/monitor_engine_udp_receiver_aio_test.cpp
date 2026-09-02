#include "engine.h"
#include "monitor_data.h"
#include "monitor_wire.h"
#include "udp_receiver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace std::chrono_literals;

namespace {

/*
 * 案例 1：固定 UdpReceiver 的阶段 10 公共接口。
 *
 * 测试功能：
 * 确认接收器以非拥有方式向 Engine 暴露 fd，并通过 receive_one() 返回
 * 包含统一解码结果的值对象；同时禁止复制、移动 socket 所有权。
 *
 * 测试步骤：
 * 1. 固定 receive_one() 的参数、const 属性和返回类型；
 * 2. 固定 fd()、bound_port() 的返回类型；
 * 3. 确认 UdpReceiver 不可复制、不可移动。
 */
using ExpectedReceiveOne =
    Wire::UdpReceiveResult (Wire::UdpReceiver::*)() const;
using ExpectedFd = int (Wire::UdpReceiver::*)() const noexcept;
using ExpectedBoundPort =
    std::uint16_t (Wire::UdpReceiver::*)() const noexcept;

static_assert(
    std::is_same_v<
        decltype(&Wire::UdpReceiver::receive_one),
        ExpectedReceiveOne>,
    "UdpReceiver::receive_one signature changed");
static_assert(
    std::is_same_v<decltype(&Wire::UdpReceiver::fd), ExpectedFd>,
    "UdpReceiver::fd signature changed");
static_assert(
    std::is_same_v<
        decltype(&Wire::UdpReceiver::bound_port),
        ExpectedBoundPort>,
    "UdpReceiver::bound_port signature changed");
static_assert(!std::is_copy_constructible_v<Wire::UdpReceiver>);
static_assert(!std::is_copy_assignable_v<Wire::UdpReceiver>);
static_assert(!std::is_move_constructible_v<Wire::UdpReceiver>);
static_assert(!std::is_move_assignable_v<Wire::UdpReceiver>);

/*
 * 测试发送器：所有阶段 10 数据报都通过同一个源 socket 依次发送到同一个
 * UdpReceiver，从发送侧也保持严格的 UDP 数据报顺序和边界。
 */
class RawUdpSender final {
public:
    explicit RawUdpSender(std::uint16_t destination_port)
        : _destination_port(destination_port) {
        assert(_destination_port != 0U);

        _socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        assert(_socket >= 0);
    }

    ~RawUdpSender() {
        if (_socket >= 0) {
            const int close_result = ::close(_socket);
            assert(close_result == 0);
            _socket = -1;
        }
    }

    RawUdpSender(const RawUdpSender&) = delete;
    RawUdpSender& operator=(const RawUdpSender&) = delete;
    RawUdpSender(RawUdpSender&&) = delete;
    RawUdpSender& operator=(RawUdpSender&&) = delete;

    void send(const std::vector<std::uint8_t>& bytes) const {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(_destination_port);
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        const ssize_t sent =
            ::sendto(
                _socket,
                bytes.data(),
                bytes.size(),
                0,
                reinterpret_cast<const sockaddr*>(&destination),
                static_cast<socklen_t>(sizeof(destination)));

        assert(sent >= 0);
        assert(static_cast<std::size_t>(sent) == bytes.size());
    }

private:
    int _socket{-1};
    const std::uint16_t _destination_port;
};

/*
 * AIO 回调写入、测试主线程读取，因此结果列表由 mutex 保护；条件变量用于
 * 等待精确的接收数量，避免用固定 sleep 猜测回调何时完成。
 */
class ReceiveCollector final {
public:
    void push(Wire::UdpReceiveResult result) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _results.push_back(std::move(result));
        }

        _condition.notify_all();
    }

    [[nodiscard]] bool wait_for_size(
        std::size_t expected,
        std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(_mutex);

        return _condition.wait_for(
            lock,
            timeout,
            [&] {
                return _results.size() >= expected;
            });
    }

    [[nodiscard]] std::vector<Wire::UdpReceiveResult> snapshot() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _results;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::vector<Wire::UdpReceiveResult> _results;
};

/*
 * 等待 Engine 进入目标生命周期状态，不使用固定 sleep。
 */
bool wait_for_phase(
    Engine& engine,
    EnginePhase expected,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == expected) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == expected;
}

/*
 * 等待接收 socket 可读，只用于不启动 Engine 的 receive_one() 边界测试。
 */
bool wait_for_readable(int fd, std::chrono::milliseconds timeout) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;

    const int result =
        ::poll(
            &descriptor,
            1U,
            static_cast<int>(timeout.count()));
    assert(result >= 0);

    return result > 0 &&
           (descriptor.revents & POLLIN) != 0;
}

MonData::StoredRecord make_numeric_record(
    MonData::MonitorKey key,
    std::uint32_t value,
    std::uint32_t state,
    std::string description,
    MonData::MonitorTimestamp timestamp) {
    return MonData::StoredRecord{
        MonData::MonitorData{
            std::move(key),
            std::move(description),
            MonData::NumericValue{value, state}},
        timestamp};
}

MonData::StoredRecord make_string_record(
    MonData::MonitorKey key,
    std::string value,
    std::string description,
    MonData::MonitorTimestamp timestamp) {
    return MonData::StoredRecord{
        MonData::MonitorData{
            std::move(key),
            std::move(description),
            std::move(value)},
        timestamp};
}

std::vector<std::uint8_t> encode_bytes(
    const MonData::StoredRecord& record,
    Wire::WireVersion version) {
    Wire::EncodeResult encoded = Wire::encode(record, version);
    assert(encoded._status == Wire::WireStatus::SUCCESS);
    assert(!encoded._bytes.empty());
    return std::move(encoded._bytes);
}

const MonData::NumericValue& numeric_value(
    const Wire::UdpReceiveResult& result) {
    assert(result._record.has_value());
    return std::get<MonData::NumericValue>(
        result._record->_data._value);
}

const std::string& string_value(
    const Wire::UdpReceiveResult& result) {
    assert(result._record.has_value());
    return std::get<std::string>(
        result._record->_data._value);
}

/*
 * 一次 AIO 激活要把 socket 当前队列读空。
 *
 * SUCCESS、DECODE_ERROR 和 DATAGRAM_TOO_LARGE 都表示一个数据报已经被消费，
 * 因此保存结果后继续读。WOULD_BLOCK 是正常结束条件。未知版本只形成一个
 * DECODE_ERROR，绝不能关闭 fd、移除 AIO 或停止后续 drain。
 */
int drain_udp_receiver(
    const std::shared_ptr<Wire::UdpReceiver>& receiver,
    const std::shared_ptr<ReceiveCollector>& collector) {
    for (;;) {
        Wire::UdpReceiveResult result = receiver->receive_one();

        if (result._status == Wire::UdpReceiveStatus::WOULD_BLOCK) {
            return 0;
        }

        const bool terminal_receive_error =
            result._status == Wire::UdpReceiveStatus::INVALID_ENDPOINT ||
            result._status == Wire::UdpReceiveStatus::SOCKET_ERROR ||
            result._status == Wire::UdpReceiveStatus::RECEIVE_ERROR;

        collector->push(std::move(result));

        if (terminal_receive_error) {
            return -1;
        }
    }
}

/*
 * 案例 2：接收 socket 是非阻塞、close-on-exec，并正确返回临时端口。
 *
 * 测试功能：
 * 防止水平触发 AIO 重复激活后第二次 recvfrom() 阻塞；同时确认端口 0 会被
 * 系统替换为真实非零端口，供 Publisher/测试发送器使用。
 *
 * 测试步骤：
 * 1. 在 127.0.0.1:0 创建 UdpReceiver；
 * 2. 检查 ready、fd 和实际绑定端口；
 * 3. 读取 fd 标志，确认 O_NONBLOCK 和 FD_CLOEXEC；
 * 4. 空队列调用 receive_one()，确认立即返回 WOULD_BLOCK。
 */
void test_receiver_is_nonblocking_and_reports_bound_port() {
    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});

    assert(receiver.ready());
    assert(receiver.setup_status() == Wire::UdpReceiveStatus::SUCCESS);
    assert(receiver.fd() >= 0);
    assert(receiver.bound_port() != 0U);

    const int status_flags = ::fcntl(receiver.fd(), F_GETFL, 0);
    assert(status_flags >= 0);
    assert((status_flags & O_NONBLOCK) != 0);

    const int descriptor_flags = ::fcntl(receiver.fd(), F_GETFD, 0);
    assert(descriptor_flags >= 0);
    assert((descriptor_flags & FD_CLOEXEC) != 0);

    const Wire::UdpReceiveResult empty = receiver.receive_one();
    assert(empty._status == Wire::UdpReceiveStatus::WOULD_BLOCK);
    assert(empty._wire_status == Wire::WireStatus::SUCCESS);
    assert(!empty._record.has_value());
    assert(empty._bytes_received == 0U);
}

/*
 * 案例 3：超过 1200 字节的数据报在进入 Codec 前被拒绝。
 *
 * 测试功能：
 * 接收缓冲区必须能够发现 1201 字节数据报，不能先截断到 1200 再误当成
 * 合法 V2 报文。
 *
 * 测试步骤：
 * 1. 向接收端发送恰好 1201 字节；
 * 2. 等待 fd 可读并调用 receive_one()；
 * 3. 断言 DATAGRAM_TOO_LARGE、无记录且实际长度为 1201；
 * 4. 再次读取应返回 WOULD_BLOCK，证明该非法包已经被消费。
 */
void test_oversized_datagram_is_rejected_and_consumed() {
    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());
    assert(receiver.bound_port() != 0U);

    RawUdpSender sender(receiver.bound_port());
    sender.send(std::vector<std::uint8_t>(
        Wire::V2_MAX_DATAGRAM_SIZE + 1U,
        static_cast<std::uint8_t>(Wire::WireVersion::V2)));

    assert(wait_for_readable(receiver.fd(), 1s));

    const Wire::UdpReceiveResult oversized = receiver.receive_one();
    assert(oversized._status ==
           Wire::UdpReceiveStatus::DATAGRAM_TOO_LARGE);
    assert(oversized._wire_status ==
           Wire::WireStatus::DATAGRAM_TOO_LARGE);
    assert(!oversized._record.has_value());
    assert(oversized._bytes_received ==
           Wire::V2_MAX_DATAGRAM_SIZE + 1U);

    const Wire::UdpReceiveResult empty = receiver.receive_one();
    assert(empty._status == Wire::UdpReceiveStatus::WOULD_BLOCK);
}

/*
 * 案例 4：同一个 AIO socket 顺序接收 V1、V2、未知版本和恢复记录。
 *
 * 测试功能：
 * 完整验证阶段 10 的统一接收链路。AIO 回调只调用 receive_one()；版本分派
 * 完全由 Wire::decode() 完成。未知版本产生 WRONG_VERSION 后，同一 fd 和
 * AIO 注册仍然有效，下一条合法数据继续成功。
 *
 * 测试步骤：
 * 1. 创建一个非阻塞 UdpReceiver，并将同一个 fd 注册给 Engine AIO；
 * 2. 通过同一个 RawUdpSender 依次发送 V1 数值、V1 字符串、V2 数值、
 *    V2 长字符串和带中文 description/精确 timestamp 的 V2 记录；
 * 3. 发送未知版本 99，确认得到 WRONG_VERSION；
 * 4. 再发送合法 V1 数值，确认接收链路恢复；
 * 5. 停止 Engine 后逐项验证七个结果的顺序、值和版本元数据。
 */
void test_same_aio_socket_receives_both_versions_and_recovers() {
    auto receiver = std::make_shared<Wire::UdpReceiver>(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver->ready());
    assert(receiver->bound_port() != 0U);

    auto collector = std::make_shared<ReceiveCollector>();

    Engine engine(MonConfig{"m6-stage10-aio", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    const std::optional<AioHandle> handle =
        engine.add_aio(
            receiver->fd(),
            MonCallback{
                "m6-stage10-udp-receiver",
                [receiver, collector]() -> int {
                    return drain_udp_receiver(receiver, collector);
                },
                false});
    assert(handle.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    const bool running = wait_for_phase(engine, EnginePhase::RUNNING, 1s);
    assert(running);

    RawUdpSender sender(receiver->bound_port());
    const MonData::MonitorTimestamp timestamp{
        std::chrono::seconds{1'700'000'000} +
        std::chrono::nanoseconds{123'456'789}};

    const MonData::StoredRecord v1_numeric =
        make_numeric_record(
            {1U, 2U, 3U, 4U},
            100U,
            2U,
            "not-carried-by-v1",
            timestamp);
    const std::vector<std::uint8_t> v1_numeric_bytes =
        encode_bytes(v1_numeric, Wire::WireVersion::V1);
    sender.send(v1_numeric_bytes);
    assert(collector->wait_for_size(1U, 1s));

    const MonData::StoredRecord v1_string =
        make_string_record(
            {5U, 6U, 7U, 8U},
            "legacy-string",
            "not-carried-by-v1",
            timestamp);
    const std::vector<std::uint8_t> v1_string_bytes =
        encode_bytes(v1_string, Wire::WireVersion::V1);
    sender.send(v1_string_bytes);
    assert(collector->wait_for_size(2U, 1s));

    const MonData::StoredRecord v2_numeric =
        make_numeric_record(
            {9U, 10U, 11U, 12U},
            std::numeric_limits<std::uint32_t>::max(),
            2U,
            "v2-numeric-description",
            timestamp);
    const std::vector<std::uint8_t> v2_numeric_bytes =
        encode_bytes(v2_numeric, Wire::WireVersion::V2);
    sender.send(v2_numeric_bytes);
    assert(collector->wait_for_size(3U, 1s));

    const std::string long_value(256U, 'L');
    const MonData::StoredRecord v2_long_string =
        make_string_record(
            {13U, 14U, 15U, 16U},
            long_value,
            "v2-long-description",
            timestamp);
    const std::vector<std::uint8_t> v2_long_bytes =
        encode_bytes(v2_long_string, Wire::WireVersion::V2);
    sender.send(v2_long_bytes);
    assert(collector->wait_for_size(4U, 1s));

    const MonData::StoredRecord v2_metadata =
        make_string_record(
            {17U, 18U, 19U, 20U},
            "metadata-value",
            u8"完整中文描述",
            timestamp);
    const std::vector<std::uint8_t> v2_metadata_bytes =
        encode_bytes(v2_metadata, Wire::WireVersion::V2);
    sender.send(v2_metadata_bytes);
    assert(collector->wait_for_size(5U, 1s));

    const std::vector<std::uint8_t> unknown_version{
        99U,
        0U,
        0U,
        0U};
    sender.send(unknown_version);
    assert(collector->wait_for_size(6U, 1s));

    const MonData::StoredRecord recovery =
        make_numeric_record(
            {21U, 22U, 23U, 24U},
            200U,
            0U,
            "not-carried-by-v1",
            timestamp);
    const std::vector<std::uint8_t> recovery_bytes =
        encode_bytes(recovery, Wire::WireVersion::V1);
    sender.send(recovery_bytes);
    assert(collector->wait_for_size(7U, 1s));

    engine.stop();
    const bool run_completed =
        run_result.wait_for(1s) == std::future_status::ready;
    assert(run_completed);
    runner.join();
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const std::vector<Wire::UdpReceiveResult> results =
        collector->snapshot();
    assert(results.size() == 7U);

    /* V1 数值：保留 Key/value/state，不携带 description/timestamp。 */
    assert(results[0].success());
    assert(results[0]._bytes_received == v1_numeric_bytes.size());
    assert(results[0]._record.has_value());
    assert(results[0]._record->_version == Wire::WireVersion::V1);
    assert(results[0]._record->_data._key == v1_numeric._data._key);
    assert(numeric_value(results[0])._value == 100U);
    assert(numeric_value(results[0])._state == 2U);
    assert(results[0]._record->_data._description.empty());
    assert(!results[0]._record->_changed_at.has_value());

    /* V1 字符串：同一个 socket 自动分派到 V1 decoder。 */
    assert(results[1].success());
    assert(results[1]._bytes_received == v1_string_bytes.size());
    assert(results[1]._record.has_value());
    assert(results[1]._record->_version == Wire::WireVersion::V1);
    assert(results[1]._record->_data._key == v1_string._data._key);
    assert(string_value(results[1]) == "legacy-string");
    assert(results[1]._record->_data._description.empty());
    assert(!results[1]._record->_changed_at.has_value());

    /* V2 数值：UINT32_MAX、description 和精确 timestamp 完整恢复。 */
    assert(results[2].success());
    assert(results[2]._bytes_received == v2_numeric_bytes.size());
    assert(results[2]._record.has_value());
    assert(results[2]._record->_version == Wire::WireVersion::V2);
    assert(results[2]._record->_data._key == v2_numeric._data._key);
    assert(numeric_value(results[2])._value ==
           std::numeric_limits<std::uint32_t>::max());
    assert(numeric_value(results[2])._state == 2U);
    assert(results[2]._record->_data._description ==
           "v2-numeric-description");
    assert(results[2]._record->_changed_at.has_value());
    assert(*results[2]._record->_changed_at == timestamp);

    /* V2 长字符串：256 字节超过 V1 上限，但在 V2 中完整保留。 */
    assert(results[3].success());
    assert(results[3]._bytes_received == v2_long_bytes.size());
    assert(results[3]._record.has_value());
    assert(results[3]._record->_version == Wire::WireVersion::V2);
    assert(results[3]._record->_data._key == v2_long_string._data._key);
    assert(string_value(results[3]) == long_value);
    assert(string_value(results[3]).size() == 256U);
    assert(results[3]._record->_data._description ==
           "v2-long-description");
    assert(results[3]._record->_changed_at.has_value());
    assert(*results[3]._record->_changed_at == timestamp);

    /* V2 metadata：UTF-8 description 和纳秒时间戳按字节/数值完整恢复。 */
    assert(results[4].success());
    assert(results[4]._bytes_received == v2_metadata_bytes.size());
    assert(results[4]._record.has_value());
    assert(results[4]._record->_version == Wire::WireVersion::V2);
    assert(results[4]._record->_data._key == v2_metadata._data._key);
    assert(string_value(results[4]) == "metadata-value");
    assert(results[4]._record->_data._description == u8"完整中文描述");
    assert(results[4]._record->_changed_at.has_value());
    assert(*results[4]._record->_changed_at == timestamp);

    /* 未知版本：形成可观察错误，但不生成伪造记录。 */
    assert(results[5]._status == Wire::UdpReceiveStatus::DECODE_ERROR);
    assert(results[5]._wire_status == Wire::WireStatus::WRONG_VERSION);
    assert(results[5]._bytes_received == unknown_version.size());
    assert(!results[5]._record.has_value());

    /* 恢复记录：未知版本之后，同一个 socket/AIO 继续正常接收 V1。 */
    assert(results[6].success());
    assert(results[6]._bytes_received == recovery_bytes.size());
    assert(results[6]._record.has_value());
    assert(results[6]._record->_version == Wire::WireVersion::V1);
    assert(results[6]._record->_data._key == recovery._data._key);
    assert(numeric_value(results[6])._value == 200U);
    assert(numeric_value(results[6])._state == 0U);
}

} // namespace

int main() {
    test_receiver_is_nonblocking_and_reports_bound_port();
    test_oversized_datagram_is_rejected_and_consumed();
    test_same_aio_socket_receives_both_versions_and_recovers();

    std::cout << "M6_AIO_RECEIVER_CONTRACT=PASS\n";
    std::cout << "M6_AIO_RECEIVER_NONBLOCKING=PASS\n";
    std::cout << "M6_AIO_OVERSIZED_DATAGRAM=PASS\n";
    std::cout << "M6_AIO_V1_NUMERIC=PASS\n";
    std::cout << "M6_AIO_V1_STRING=PASS\n";
    std::cout << "M6_AIO_V2_NUMERIC=PASS\n";
    std::cout << "M6_AIO_V2_LONG_STRING=PASS\n";
    std::cout << "M6_AIO_V2_METADATA=PASS\n";
    std::cout << "M6_AIO_UNKNOWN_VERSION_RECOVERY=PASS\n";
    return 0;
}
