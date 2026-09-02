#include "engine.h"
#include "monitor_data.h"
#include "monitor_reporter.h"
#include "monitor_wire.h"
#include "udp_publisher.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

/*
 * 案例 1：固定  Publisher seam 的公共类型。
 *
 * 测试功能：
 * 确认 UDP Publisher 通过已有的按值 StoredRecord 回调接入 Engine，
 * 而不是让 Engine 直接依赖某个具体 WireVersion 或 UdpPublisher 类型。
 *
 * 测试步骤：
 * 1. 固定 Engine::MonitorPublisher 的 std::function 类型；
 * 2. 固定 Engine::set_publisher() 参数和返回类型；
 * 3. 固定 shared_ptr 是可复制的，能够被 C++17 std::function 保存。
 */
using ExpectedEnginePublisher =
    std::function<void(MonData::StoredRecord)>;

static_assert(
    std::is_same_v<
        Engine::MonitorPublisher,
        ExpectedEnginePublisher>,
    "Engine Publisher seam must accept StoredRecord by value");

using ExpectedSetPublisher =
    bool (Engine::*)(Engine::MonitorPublisher);

static_assert(
    std::is_same_v<
        decltype(&Engine::set_publisher),
        ExpectedSetPublisher>,
    "Engine::set_publisher signature changed");

using SharedUdpPublisher =
    std::shared_ptr<Wire::UdpPublisher>;

static_assert(
    std::is_copy_constructible_v<SharedUdpPublisher>,
    "shared UDP Publisher handle must be copyable");

/*
 * 本地 UDP 接收器。
 *
 * 每个案例绑定独立的 127.0.0.1 临时端口，避免不同测试间残留数据报
 * 相互影响。receive() 最多等待一秒，防止失败案例永久阻塞。
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
            ::bind(
                _socket,
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address)));
        assert(bind_result == 0);

        socklen_t address_size =
            static_cast<socklen_t>(sizeof(address));
        const int name_result =
            ::getsockname(
                _socket,
                reinterpret_cast<sockaddr*>(&address),
                &address_size);
        assert(name_result == 0);
        assert(address_size == sizeof(address));

        _port = ntohs(address.sin_port);
        assert(_port != 0U);
    }

    ~LoopbackUdpReceiver() {
        if (_socket >= 0) {
            const int close_result = ::close(_socket);
            assert(close_result == 0);
            _socket = -1;
        }
    }

    LoopbackUdpReceiver(const LoopbackUdpReceiver&) = delete;
    LoopbackUdpReceiver& operator=(const LoopbackUdpReceiver&) = delete;
    LoopbackUdpReceiver(LoopbackUdpReceiver&&) = delete;
    LoopbackUdpReceiver& operator=(LoopbackUdpReceiver&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return _port;
    }

    [[nodiscard]] bool has_datagram(int timeout_ms) const {
        pollfd descriptor{};
        descriptor.fd = _socket;
        descriptor.events = POLLIN;

        const int poll_result =
            ::poll(&descriptor, 1U, timeout_ms);
        assert(poll_result >= 0);

        if (poll_result == 0) {
            return false;
        }

        assert((descriptor.revents & POLLIN) != 0);
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> receive() const {
        const bool readable = has_datagram(1'000);
        assert(readable);

        std::array<
            std::uint8_t,
            Wire::V2_MAX_DATAGRAM_SIZE + 1U> buffer{};

        const ssize_t received =
            ::recvfrom(
                _socket,
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
    int _socket{-1};
    std::uint16_t _port{0};
};

/*
 * 向本机指定端口发送一个原始数据报，只用于独立验证测试接收器。
 */
void send_raw_loopback_datagram(
    std::uint16_t port,
    const std::vector<std::uint8_t>& bytes) {
    assert(port != 0U);
    assert(!bytes.empty());

    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(socket_fd >= 0);

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const ssize_t sent =
        ::sendto(
            socket_fd,
            bytes.data(),
            bytes.size(),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            static_cast<socklen_t>(sizeof(destination)));
    assert(sent >= 0);
    assert(static_cast<std::size_t>(sent) == bytes.size());

    const int close_result = ::close(socket_fd);
    assert(close_result == 0);
}

/*
 * 一次性读取调用次数和最后发送结果，避免并发读取两个独立字段时看到
 * 不一致状态。
 */
struct SendProbeSnapshot final {
    std::size_t _calls{0};
    std::optional<Wire::UdpPublishResult> _last_result;
};

/*
 * 线程安全的发送结果观察器。
 *
 * Engine Publisher 的类型返回 void，因此测试通过 Probe 在回调外观察
 * UdpPublisher::send() 成功、编码失败或系统发送失败的具体结果。
 */
class SendProbe final {
public:
    SendProbe() = default;
    ~SendProbe() = default;

    SendProbe(const SendProbe&) = delete;
    SendProbe& operator=(const SendProbe&) = delete;
    SendProbe(SendProbe&&) = delete;
    SendProbe& operator=(SendProbe&&) = delete;

    void record(Wire::UdpPublishResult result) {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_calls;
        _last_result = std::move(result);
    }

    [[nodiscard]] SendProbeSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return SendProbeSnapshot{_calls, _last_result};
    }

private:
    mutable std::mutex _mutex;
    std::size_t _calls{0};
    std::optional<Wire::UdpPublishResult> _last_result;
};

using SharedSendProbe = std::shared_ptr<SendProbe>;

/*
 * 建立阶段 9 的实际接线：
 *
 * Engine::set_publisher()
 *          ↓
 * 捕获 shared_ptr<UdpPublisher> 的可复制 lambda
 *          ↓
 * UdpPublisher::send(record)
 *
 * probe 为空时只发送；非空时额外保存发送结果。注册失败或 UDP Publisher
 * 初始化失败时返回空 shared_ptr。
 */
SharedUdpPublisher install_udp_publisher(
    Engine& engine,
    Wire::UdpPublisherConfig config,
    SharedSendProbe probe = {}) {
    SharedUdpPublisher publisher =
        std::make_shared<Wire::UdpPublisher>(
            std::move(config));

    if (!publisher->ready()) {
        return {};
    }

    Engine::MonitorPublisher callback =
        [publisher, probe](MonData::StoredRecord record) {
            Wire::UdpPublishResult result =
                publisher->send(record);

            if (probe) {
                probe->record(std::move(result));
            }
        };

    const bool registered =
        engine.set_publisher(std::move(callback));

    if (!registered) {
        return {};
    }

    return publisher;
}

const MonData::NumericValue& numeric_value(
    const MonData::StoredRecord& record) {
    return std::get<MonData::NumericValue>(
        record._data._value);
}

const std::string& string_value(
    const MonData::StoredRecord& record) {
    return std::get<std::string>(record._data._value);
}

const MonData::NumericValue& decoded_numeric_value(
    const Wire::DecodedRecord& record) {
    return std::get<MonData::NumericValue>(
        record._data._value);
}

const std::string& decoded_string_value(
    const Wire::DecodedRecord& record) {
    return std::get<std::string>(record._data._value);
}

/*
 * 案例 2：Engine 回调拥有 UdpPublisher 生命周期。
 *
 * 测试功能：
 * 验证调用者释放自己的 shared_ptr 后，Engine 内保存的 lambda 仍保持 UDP
 * Publisher 有效；Engine 析构时最后一个引用自动释放。
 *
 * 测试步骤：
 * 1. 初始化 READY Engine 并注册 V1 Publisher；
 * 2. 保存 weak_ptr 后释放局部 shared_ptr；
 * 3. Engine 存活时 weak_ptr 不过期；
 * 4. Engine 析构后 weak_ptr 过期。
 */
void test_engine_callback_owns_udp_publisher_lifetime() {
    std::weak_ptr<Wire::UdpPublisher> weak_publisher;

    {
        Engine engine(MonConfig{
            "m6-stage9-lifetime",
            9000U,
            1U});
        const ENGINESTATE init_result = engine.init();
        assert(init_result == ENGINESTATE::SUCCESSFUL);

        SharedUdpPublisher publisher =
            install_udp_publisher(
                engine,
                Wire::UdpPublisherConfig{
                    {"127.0.0.1", 9100U},
                    Wire::WireVersion::V1});
        assert(publisher != nullptr);

        weak_publisher = publisher;
        assert(publisher.use_count() >= 2L);

        publisher.reset();
        assert(!weak_publisher.expired());
    }

    assert(weak_publisher.expired());
}

/*
 * 案例 3：真实 loopback 接收器保持单个 UDP 数据报边界。
 *
 * 测试功能：
 * 排除测试辅助设施本身的问题，确认它能超时、接收内嵌零字节的数据报，
 * 并在读取后报告队列为空。
 *
 * 测试步骤：
 * 1. 发送前确认没有数据；
 * 2. 用独立 socket 调用一次 sendto()；
 * 3. 逐字节比较收到的数据；
 * 4. 确认不存在第二个数据报。
 */
void test_loopback_receiver_reads_one_complete_datagram() {
    LoopbackUdpReceiver receiver;
    assert(!receiver.has_datagram(20));

    const std::vector<std::uint8_t> expected{
        0x02U,
        0xaaU,
        0x00U,
        0xffU};
    send_raw_loopback_datagram(receiver.port(), expected);

    const std::vector<std::uint8_t> received = receiver.receive();
    assert(received == expected);
    assert(!receiver.has_datagram(50));
}

/*
 * 案例 4：SendProbe 保存调用次数和最近结果。
 *
 * 测试功能：
 * 验证 Probe 初始为空，记录成功后 calls=1，再记录失败后 calls=2 且最近
 * 结果被失败结果替换。
 */
void test_send_probe_records_latest_result() {
    SendProbe probe;

    const SendProbeSnapshot initial = probe.snapshot();
    assert(initial._calls == 0U);
    assert(!initial._last_result.has_value());

    probe.record(Wire::UdpPublishResult{
        Wire::UdpPublishStatus::SUCCESS,
        Wire::WireStatus::SUCCESS,
        Wire::V1_NUMERIC_DATAGRAM_SIZE,
        0});

    const SendProbeSnapshot after_success = probe.snapshot();
    assert(after_success._calls == 1U);
    assert(after_success._last_result.has_value());
    assert(after_success._last_result->success());
    assert(after_success._last_result->_bytes_sent ==
           Wire::V1_NUMERIC_DATAGRAM_SIZE);

    probe.record(Wire::UdpPublishResult{
        Wire::UdpPublishStatus::SEND_ERROR,
        Wire::WireStatus::SUCCESS,
        0U,
        13});

    const SendProbeSnapshot after_failure = probe.snapshot();
    assert(after_failure._calls == 2U);
    assert(after_failure._last_result.has_value());
    assert(after_failure._last_result->_status ==
           Wire::UdpPublishStatus::SEND_ERROR);
    assert(after_failure._last_result->_system_error == 13);
}

/*
 * 案例 5：M4 数值去重作用于 V1 Publisher。
 *
 * 测试功能：
 * INSERTED 和 UPDATED 各发送一个 V1 数据报；相同数值即使 description 和
 * timestamp 变化仍为 UNCHANGED，不进入 UDP Publisher。
 *
 * 测试步骤：
 * 1. 上报 10，断言 INSERTED 并接收 V1；
 * 2. 再上报 10，断言 UNCHANGED、Probe 次数不变且没有数据报；
 * 3. 上报 20，断言 UPDATED 并接收第二个 V1；
 * 4. 查询 Store，确认保存最新值和 description。
 */
void test_v1_numeric_dedup_only_sends_changes() {
    LoopbackUdpReceiver receiver;
    Engine engine(MonConfig{"m6-stage9-v1-dedup", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    const SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"127.0.0.1", receiver.port()},
                Wire::WireVersion::V1},
            probe);
    assert(publisher != nullptr);

    const MonData::MonitorKey key{1U, 2U, 3U, 4U};

    const MonData::UpdateResult inserted =
        engine.report_count(key, 10U, "inserted");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const std::vector<std::uint8_t> inserted_datagram = receiver.receive();
    const Wire::DecodeResult inserted_decoded =
        Wire::decode(inserted_datagram.data(), inserted_datagram.size());
    assert(inserted_decoded._status == Wire::WireStatus::SUCCESS);
    assert(inserted_decoded._record.has_value());
    assert(inserted_decoded._record->_version == Wire::WireVersion::V1);
    assert(decoded_numeric_value(*inserted_decoded._record)._value == 10U);

    const MonData::UpdateResult unchanged =
        engine.report_count(key, 10U, "metadata-only-change");
    assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
    assert(probe->snapshot()._calls == 1U);
    assert(!receiver.has_datagram(100));

    const MonData::UpdateResult updated =
        engine.report_count(key, 20U, "updated");
    assert(updated._status == MonData::UpdateStatus::UPDATED);

    const std::vector<std::uint8_t> updated_datagram = receiver.receive();
    const Wire::DecodeResult updated_decoded =
        Wire::decode(updated_datagram.data(), updated_datagram.size());
    assert(updated_decoded._status == Wire::WireStatus::SUCCESS);
    assert(updated_decoded._record.has_value());
    assert(updated_decoded._record->_version == Wire::WireVersion::V1);
    assert(decoded_numeric_value(*updated_decoded._record)._value == 20U);
    assert(probe->snapshot()._calls == 2U);
    assert(!receiver.has_datagram(50));

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 20U);
    assert(stored->_data._description == "updated");
}

/*
 * 案例 6：M4 字符串去重作用于 V2 Publisher。
 *
 * 测试功能：
 * 证明去重门位于 WireVersion 选择之前；相同完整字符串不发送 V2，不同
 * 字符串发送并保留 V2 description 和 timestamp。
 *
 * 测试步骤：
 * 1. 首次上报 "ready" 并接收 V2；
 * 2. 重复 "ready"，断言 UNCHANGED 且无数据报；
 * 3. 改为 "running"，断言 UPDATED 并解码完整 V2 元数据。
 */
void test_v2_string_dedup_only_sends_changes() {
    LoopbackUdpReceiver receiver;
    Engine engine(MonConfig{"m6-stage9-v2-dedup", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    const SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"127.0.0.1", receiver.port()},
                Wire::WireVersion::V2},
            probe);
    assert(publisher != nullptr);

    const MonData::MonitorKey key{5U, 6U, 7U, 8U};

    const MonData::UpdateResult inserted =
        engine.report_string(key, "ready", "inserted");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    const std::vector<std::uint8_t> inserted_datagram = receiver.receive();
    assert(inserted_datagram.front() ==
           static_cast<std::uint8_t>(Wire::WireVersion::V2));

    const MonData::UpdateResult unchanged =
        engine.report_string(key, "ready", "metadata-only-change");
    assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
    assert(probe->snapshot()._calls == 1U);
    assert(!receiver.has_datagram(100));

    const MonData::UpdateResult updated =
        engine.report_string(key, "running", "updated");
    assert(updated._status == MonData::UpdateStatus::UPDATED);

    const std::vector<std::uint8_t> updated_datagram = receiver.receive();
    const Wire::DecodeResult decoded =
        Wire::decode(updated_datagram.data(), updated_datagram.size());
    assert(decoded._status == Wire::WireStatus::SUCCESS);
    assert(decoded._record.has_value());
    assert(decoded._record->_version == Wire::WireVersion::V2);
    assert(decoded._record->_data._key == key);
    assert(decoded._record->_data._description == "updated");
    assert(decoded_string_value(*decoded._record) == "running");
    assert(decoded._record->_changed_at.has_value());
    assert(probe->snapshot()._calls == 2U);
    assert(!receiver.has_datagram(50));
}

/*
 * 案例 7：V1 超长字符串编码失败，但 Store 不回滚。
 *
 * 测试功能：
 * 64 字节字符串超过 V1 63 字节限制。Reporter 已经 INSERTED 后才调用 UDP
 * Publisher，因此编码失败只影响投递，不删除或截断 Store 中的数据。
 *
 * 测试步骤：
 * 1. 上报 64 字节字符串；
 * 2. 断言 Engine 返回 INSERTED；
 * 3. Probe 观察到 ENCODE_ERROR/STRING_TOO_LONG；
 * 4. 接收端无数据报；
 * 5. Store 仍保存完整 64 字节字符串。
 */
void test_v1_long_string_is_stored_but_not_sent() {
    LoopbackUdpReceiver receiver;
    Engine engine(MonConfig{"m6-stage9-v1-long", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    const SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"127.0.0.1", receiver.port()},
                Wire::WireVersion::V1},
            probe);
    assert(publisher != nullptr);

    const MonData::MonitorKey key{9U, 10U, 11U, 12U};
    const std::string long_value(64U, 'X');

    const MonData::UpdateResult inserted =
        engine.report_string(key, long_value, "v1-too-long");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const SendProbeSnapshot snapshot = probe->snapshot();
    assert(snapshot._calls == 1U);
    assert(snapshot._last_result.has_value());
    assert(snapshot._last_result->_status ==
           Wire::UdpPublishStatus::ENCODE_ERROR);
    assert(snapshot._last_result->_wire_status ==
           Wire::WireStatus::STRING_TOO_LONG);
    assert(snapshot._last_result->_bytes_sent == 0U);
    assert(!receiver.has_datagram(100));

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(string_value(*stored) == long_value);
    assert(string_value(*stored).size() == 64U);
}

/*
 * 案例 8：相同 64 字节字符串使用 V2 可以完整发送。
 *
 * 测试功能：
 * 证明 63 字节限制只属于 V1 Codec，不属于 M4 Store、M5 Reporter 或 V2。
 *
 * 测试步骤：
 * 1. 使用新 Engine 和 V2 Publisher 上报同样的 64 字节字符串；
 * 2. 断言 Store INSERTED 且 Probe 发送成功；
 * 3. 解码实际 UDP 数据报并比较完整字符串、description 和时间戳。
 */
void test_same_long_string_is_sent_by_v2() {
    LoopbackUdpReceiver receiver;
    Engine engine(MonConfig{"m6-stage9-v2-long", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    const SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"127.0.0.1", receiver.port()},
                Wire::WireVersion::V2},
            probe);
    assert(publisher != nullptr);

    const MonData::MonitorKey key{13U, 14U, 15U, 16U};
    const std::string long_value(64U, 'X');

    const MonData::UpdateResult inserted =
        engine.report_string(key, long_value, "v2-long");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const SendProbeSnapshot snapshot = probe->snapshot();
    assert(snapshot._calls == 1U);
    assert(snapshot._last_result.has_value());
    assert(snapshot._last_result->success());

    const std::vector<std::uint8_t> datagram = receiver.receive();
    const Wire::DecodeResult decoded =
        Wire::decode(datagram.data(), datagram.size());
    assert(decoded._status == Wire::WireStatus::SUCCESS);
    assert(decoded._record.has_value());
    assert(decoded._record->_version == Wire::WireVersion::V2);
    assert(decoded._record->_data._description == "v2-long");
    assert(decoded_string_value(*decoded._record) == long_value);
    assert(decoded._record->_changed_at.has_value());
    assert(!receiver.has_datagram(50));
}

/*
 * 案例 9：网络 sendto() 失败不回滚 Store。
 *
 * 测试功能：
 * 使用未开启 SO_BROADCAST 的 UDP socket 向 255.255.255.255 发送，Linux
 * 应拒绝 sendto()。Reporter 已经提交的 INSERTED 结果必须继续保留。
 *
 * 测试步骤：
 * 1. 创建指向广播地址的可用 V2 Publisher；
 * 2. 上报非零计数；
 * 3. Probe 观察到 SEND_ERROR 和非零系统错误码；
 * 4. Engine 仍返回 INSERTED，Store 仍能查到完整记录。
 */
void test_network_send_failure_does_not_rollback_store() {
    Engine engine(MonConfig{"m6-stage9-send-failure", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    const SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"255.255.255.255", 9100U},
                Wire::WireVersion::V2},
            probe);
    assert(publisher != nullptr);
    assert(publisher->ready());

    const MonData::MonitorKey key{17U, 18U, 19U, 20U};
    const MonData::UpdateResult inserted =
        engine.report_count(key, 100U, "network-failure");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    assert(inserted._record.has_value());

    const SendProbeSnapshot snapshot = probe->snapshot();
    assert(snapshot._calls == 1U);
    assert(snapshot._last_result.has_value());
    assert(snapshot._last_result->_status ==
           Wire::UdpPublishStatus::SEND_ERROR);
    assert(snapshot._last_result->_bytes_sent == 0U);
    assert(snapshot._last_result->_system_error != 0);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 100U);
    assert(stored->_data._description == "network-failure");
}

/*
 * 案例 10：注销 Publisher 后停止后续 UDP 发送，但 Store 继续更新。
 *
 * 测试功能：
 * 验证 set_publisher({}) 释放旧 lambda 捕获的 UdpPublisher；后续 UPDATED
 * 不产生数据报，同时 Store 保存最新值。
 *
 * 测试步骤：
 * 1. 注册 V2 Publisher，首次上报并接收一个数据报；
 * 2. 释放测试线程自己的 shared_ptr，确认 Engine 仍持有 Publisher；
 * 3. 调用 set_publisher({})，确认 weak_ptr 过期；
 * 4. 上报变化值，断言 UPDATED、Probe 次数不变且没有新数据报；
 * 5. 查询 Store，确认保存注销后的新值。
 */
void test_unregister_stops_future_udp_delivery() {
    LoopbackUdpReceiver receiver;
    Engine engine(MonConfig{"m6-stage9-unregister", 9000U, 1U});
    const ENGINESTATE init_result = engine.init();
    assert(init_result == ENGINESTATE::SUCCESSFUL);

    auto probe = std::make_shared<SendProbe>();
    SharedUdpPublisher publisher =
        install_udp_publisher(
            engine,
            Wire::UdpPublisherConfig{
                {"127.0.0.1", receiver.port()},
                Wire::WireVersion::V2},
            probe);
    assert(publisher != nullptr);

    std::weak_ptr<Wire::UdpPublisher> weak_publisher = publisher;
    const MonData::MonitorKey key{21U, 22U, 23U, 24U};

    const MonData::UpdateResult inserted =
        engine.report_count(key, 1U, "before-unregister");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    (void)receiver.receive();
    assert(probe->snapshot()._calls == 1U);

    publisher.reset();
    assert(!weak_publisher.expired());

    const bool unregistered = engine.set_publisher({});
    assert(unregistered);
    assert(weak_publisher.expired());

    const MonData::UpdateResult updated =
        engine.report_count(key, 2U, "after-unregister");
    assert(updated._status == MonData::UpdateStatus::UPDATED);
    assert(probe->snapshot()._calls == 1U);
    assert(!receiver.has_datagram(100));

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 2U);
    assert(stored->_data._description == "after-unregister");
}

} // namespace

int main() {
    test_engine_callback_owns_udp_publisher_lifetime();
    test_loopback_receiver_reads_one_complete_datagram();
    test_send_probe_records_latest_result();
    test_v1_numeric_dedup_only_sends_changes();
    test_v2_string_dedup_only_sends_changes();
    test_v1_long_string_is_stored_but_not_sent();
    test_same_long_string_is_sent_by_v2();
    test_network_send_failure_does_not_rollback_store();
    test_unregister_stops_future_udp_delivery();

    std::cout << "M6_STAGE9_SEAM_CONTRACT=PASS\n";
    std::cout << "M6_STAGE9_SHARED_LIFETIME=PASS\n";
    std::cout << "M6_STAGE9_LOOPBACK_RECEIVER=PASS\n";
    std::cout << "M6_STAGE9_SEND_PROBE=PASS\n";
    std::cout << "M6_ENGINE_UDP_V1_DEDUP=PASS\n";
    std::cout << "M6_ENGINE_UDP_V2_DEDUP=PASS\n";
    std::cout << "M6_ENGINE_UDP_V1_LONG_STRING=PASS\n";
    std::cout << "M6_ENGINE_UDP_V2_LONG_STRING=PASS\n";
    std::cout << "M6_ENGINE_UDP_FAILURE_STORE_PRESERVED=PASS\n";
    std::cout << "M6_ENGINE_UDP_UNREGISTER=PASS\n";
    return 0;
}
