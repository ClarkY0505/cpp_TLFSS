#include "engine.h"
#include "monitor_data.h"
#include "monitor_wire.h"
#include "udp_publisher.h"
#include "udp_receiver.h"

#include <sys/socket.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace std::chrono_literals;

namespace {

/*
 * 让一组发送线程在同一个逻辑起点开始工作。
 *
 * C++17 没有 std::barrier，因此用 mutex/condition_variable 实现一次性门闩。
 * 这不能保证线程在同一条机器指令上开始，但可以避免创建线程的先后顺序把
 * 并发测试退化成基本串行的测试。
 */
class StartGate final {
public:
    explicit StartGate(std::size_t participants)
        : _participants(participants) {
        assert(_participants != 0U);
    }

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_arrived;

        if (_arrived == _participants) {
            _open = true;
            _condition.notify_all();
            return;
        }

        _condition.wait(lock, [&] {
            return _open;
        });
    }

private:
    const std::size_t _participants;
    std::size_t _arrived{0U};
    bool _open{false};
    std::mutex _mutex;
    std::condition_variable _condition;
};

/*
 * 增大测试 socket 的接收缓存。
 *
 * UDP 本身允许在接收方来不及读取时丢包。阶段 11 要验证的是 Publisher 的
 * 并发和数据报边界，而不是故意制造接收缓存溢出，因此测试同时启动接收线程
 * 并适当增大 SO_RCVBUF，减少环境负载导致的偶发丢包。
 */
void enlarge_receive_buffer(const Wire::UdpReceiver& receiver) {
    const int requested_size = 4 * 1024 * 1024;
    const int result = ::setsockopt(
        receiver.fd(),
        SOL_SOCKET,
        SO_RCVBUF,
        &requested_size,
        static_cast<socklen_t>(sizeof(requested_size)));
    assert(result == 0);
}

/*
 * 在限定时间内接收指定数量的数据报。
 *
 * receive_one() 是非阻塞接口。WOULD_BLOCK 只表示当前队列暂时为空，发送线程
 * 仍可能继续产生数据，所以这里短暂让出 CPU 后继续等到数量满足或超时。
 * 协议错误也会作为结果保存，交给测试主体断言，避免错误数据报被静默忽略。
 */
std::vector<Wire::UdpReceiveResult> receive_exact(
    Wire::UdpReceiver& receiver,
    std::size_t expected_count,
    std::chrono::seconds timeout) {
    std::vector<Wire::UdpReceiveResult> results;
    results.reserve(expected_count);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (results.size() < expected_count &&
           std::chrono::steady_clock::now() < deadline) {
        Wire::UdpReceiveResult result = receiver.receive_one();

        if (result._status == Wire::UdpReceiveStatus::WOULD_BLOCK) {
            std::this_thread::yield();
            continue;
        }

        results.push_back(std::move(result));
    }

    return results;
}

MonData::MonitorTimestamp timestamp_for(std::size_t id) {
    return MonData::MonitorTimestamp{
        std::chrono::seconds{1'700'000'000 +
                             static_cast<std::int64_t>(id)} +
        std::chrono::nanoseconds{
            static_cast<std::int64_t>(id % 1'000'000'000U)}};
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

void assert_successful_receive(const Wire::UdpReceiveResult& result) {
    assert(result._status == Wire::UdpReceiveStatus::SUCCESS);
    assert(result._wire_status == Wire::WireStatus::SUCCESS);
    assert(result._record.has_value());
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
    return std::get<std::string>(result._record->_data._value);
}

/*
 * 案例 1：同一个 V1 Publisher 被多个线程同时调用。
 *
 * 测试功能：
 * 验证 UdpPublisher::send() 不依赖可变的共享编码缓冲区，并且同一个 UDP
 * socket 在多线程 sendto() 时仍保持每条 V1 数值记录为独立的 26 字节包。
 *
 * 测试步骤：
 * 1. 创建一个 V1 Publisher 和一个同时工作的接收线程；
 * 2. 8 个线程共享该 Publisher，每个线程发送 32 个唯一 Key；
 * 3. 检查所有 send() 成功；
 * 4. 检查收到 256 个 V1 数值包，每包 26 字节且 Key 不重复。
 */
void test_v1_publisher_concurrent_send() {
    constexpr std::size_t thread_count = 8U;
    constexpr std::size_t sends_per_thread = 32U;
    constexpr std::size_t expected_count =
        thread_count * sends_per_thread;

    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());
    enlarge_receive_buffer(receiver);

    auto publisher = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V1});
    assert(publisher->ready());

    auto receiver_future = std::async(
        std::launch::async,
        [&receiver] {
            return receive_exact(receiver, expected_count, 5s);
        });

    StartGate gate(thread_count);
    std::atomic<std::size_t> send_failures{0U};
    std::vector<std::thread> senders;
    senders.reserve(thread_count);

    for (std::size_t thread_index = 0U;
         thread_index < thread_count;
         ++thread_index) {
        senders.emplace_back([&, thread_index, publisher] {
            gate.arrive_and_wait();

            for (std::size_t sequence = 0U;
                 sequence < sends_per_thread;
                 ++sequence) {
                const std::size_t id =
                    thread_index * sends_per_thread + sequence;
                const MonData::StoredRecord record = make_numeric_record(
                    MonData::MonitorKey{
                        1U,
                        static_cast<std::uint32_t>(thread_index),
                        static_cast<std::uint32_t>(sequence),
                        1U},
                    static_cast<std::uint32_t>(id + 1U),
                    static_cast<std::uint32_t>(id % 3U),
                    "v1-description-not-encoded",
                    timestamp_for(id));

                const Wire::UdpPublishResult result =
                    publisher->send(record);
                if (!result.success()) {
                    send_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::thread& sender : senders) {
        sender.join();
    }

    const std::vector<Wire::UdpReceiveResult> results =
        receiver_future.get();

    assert(send_failures.load(std::memory_order_relaxed) == 0U);
    assert(results.size() == expected_count);

    std::set<MonData::MonitorKey> seen_keys;
    for (const Wire::UdpReceiveResult& result : results) {
        assert_successful_receive(result);
        assert(result._record->_version == Wire::WireVersion::V1);
        assert(result._bytes_received ==
               Wire::V1_NUMERIC_DATAGRAM_SIZE);
        assert(std::holds_alternative<MonData::NumericValue>(
            result._record->_data._value));
        assert(numeric_value(result)._value != 0U);
        assert(result._record->_data._description.empty());
        assert(!result._record->_changed_at.has_value());
        assert(seen_keys.insert(result._record->_data._key).second);
    }

    assert(seen_keys.size() == expected_count);
}

/*
 * 案例 2：同一个 V2 Publisher 被多个线程同时调用。
 *
 * 测试功能：
 * 除了 socket 并发，还覆盖 V2 变长 description、字符串和时间戳编码。每条
 * 接收记录都必须能根据唯一 Key 找回对应期望值，不能发生共享缓冲区串写。
 *
 * 测试步骤：
 * 1. 预先构造 192 条具有不同 Key、description、value、timestamp 的记录；
 * 2. 8 个线程共享一个 V2 Publisher 并发发送；
 * 3. 按 Key 对照全部解码字段；
 * 4. 检查数据报长度与单独 encode() 得到的长度一致。
 */
void test_v2_publisher_concurrent_send() {
    constexpr std::size_t thread_count = 8U;
    constexpr std::size_t sends_per_thread = 24U;
    constexpr std::size_t expected_count =
        thread_count * sends_per_thread;

    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());
    enlarge_receive_buffer(receiver);

    auto publisher = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V2});
    assert(publisher->ready());

    std::vector<std::vector<MonData::StoredRecord>> batches(thread_count);
    std::map<MonData::MonitorKey, MonData::StoredRecord> expected;
    std::map<MonData::MonitorKey, std::size_t> expected_sizes;

    for (std::size_t thread_index = 0U;
         thread_index < thread_count;
         ++thread_index) {
        batches[thread_index].reserve(sends_per_thread);

        for (std::size_t sequence = 0U;
             sequence < sends_per_thread;
             ++sequence) {
            const std::size_t id =
                thread_index * sends_per_thread + sequence;
            std::string value =
                "v2-thread-" + std::to_string(thread_index) +
                "-value-" + std::to_string(sequence) + "-" +
                std::string(64U + sequence % 32U, 'V');

            if (sequence % 5U == 0U) {
                value.push_back('\0');
                value.append("embedded-nul-tail");
            }

            MonData::StoredRecord record = make_string_record(
                MonData::MonitorKey{
                    2U,
                    static_cast<std::uint32_t>(thread_index),
                    static_cast<std::uint32_t>(sequence),
                    2U},
                std::move(value),
                "v2-description-" +
                    std::to_string(thread_index) + "-" +
                    std::to_string(sequence),
                timestamp_for(10'000U + id));

            const Wire::EncodeResult encoded =
                Wire::encode(record, Wire::WireVersion::V2);
            assert(encoded._status == Wire::WireStatus::SUCCESS);

            expected_sizes.emplace(
                record._data._key, encoded._bytes.size());
            expected.emplace(record._data._key, record);
            batches[thread_index].push_back(std::move(record));
        }
    }

    auto receiver_future = std::async(
        std::launch::async,
        [&receiver] {
            return receive_exact(receiver, expected_count, 5s);
        });

    StartGate gate(thread_count);
    std::atomic<std::size_t> send_failures{0U};
    std::vector<std::thread> senders;
    senders.reserve(thread_count);

    for (std::size_t thread_index = 0U;
         thread_index < thread_count;
         ++thread_index) {
        senders.emplace_back([&, thread_index, publisher] {
            gate.arrive_and_wait();

            for (const MonData::StoredRecord& record :
                 batches[thread_index]) {
                const Wire::UdpPublishResult result =
                    publisher->send(record);
                if (!result.success()) {
                    send_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::thread& sender : senders) {
        sender.join();
    }

    const std::vector<Wire::UdpReceiveResult> results =
        receiver_future.get();

    assert(send_failures.load(std::memory_order_relaxed) == 0U);
    assert(results.size() == expected_count);

    std::set<MonData::MonitorKey> seen_keys;
    for (const Wire::UdpReceiveResult& result : results) {
        assert_successful_receive(result);
        assert(result._record->_version == Wire::WireVersion::V2);

        const MonData::MonitorKey& key = result._record->_data._key;
        const auto expected_record = expected.find(key);
        const auto expected_size = expected_sizes.find(key);
        assert(expected_record != expected.end());
        assert(expected_size != expected_sizes.end());
        assert(seen_keys.insert(key).second);

        assert(result._bytes_received == expected_size->second);
        assert(result._record->_data._description ==
               expected_record->second._data._description);
        assert(string_value(result) ==
               std::get<std::string>(
                   expected_record->second._data._value));
        assert(result._record->_changed_at.has_value());
        assert(*result._record->_changed_at ==
               expected_record->second._changed_at);
    }

    assert(seen_keys.size() == expected_count);
}

/*
 * 案例 3：V1、V2 两个 Publisher 同时向同一个接收端发送。
 *
 * 测试功能：
 * 验证接收端面对两个源 socket 的并发混合流量时，仍能逐数据报调用统一
 * decode() 并正确区分版本。不同 socket 间不要求保持全局发送顺序。
 *
 * 测试步骤：
 * 1. V1 和 V2 Publisher 指向同一个 UdpReceiver；
 * 2. 两组各 4 个线程并发发送；
 * 3. 按 _mid 区分预期版本；
 * 4. 检查两个版本的数量、Key 唯一性和总数量。
 */
void test_v1_and_v2_publishers_share_receiver() {
    constexpr std::size_t threads_per_version = 4U;
    constexpr std::size_t sends_per_thread = 24U;
    constexpr std::size_t expected_per_version =
        threads_per_version * sends_per_thread;
    constexpr std::size_t expected_count =
        expected_per_version * 2U;

    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());
    enlarge_receive_buffer(receiver);

    auto v1 = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V1});
    auto v2 = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V2});
    assert(v1->ready());
    assert(v2->ready());

    auto receiver_future = std::async(
        std::launch::async,
        [&receiver] {
            return receive_exact(receiver, expected_count, 5s);
        });

    StartGate gate(threads_per_version * 2U);
    std::atomic<std::size_t> send_failures{0U};
    std::vector<std::thread> senders;
    senders.reserve(threads_per_version * 2U);

    for (std::size_t thread_index = 0U;
         thread_index < threads_per_version;
         ++thread_index) {
        senders.emplace_back([&, thread_index, v1] {
            gate.arrive_and_wait();

            for (std::size_t sequence = 0U;
                 sequence < sends_per_thread;
                 ++sequence) {
                const MonData::StoredRecord record = make_numeric_record(
                    MonData::MonitorKey{
                        11U,
                        static_cast<std::uint32_t>(thread_index),
                        static_cast<std::uint32_t>(sequence),
                        1U},
                    static_cast<std::uint32_t>(sequence + 1U),
                    0U,
                    "v1",
                    timestamp_for(sequence));

                if (!v1->send(record).success()) {
                    send_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        });

        senders.emplace_back([&, thread_index, v2] {
            gate.arrive_and_wait();

            for (std::size_t sequence = 0U;
                 sequence < sends_per_thread;
                 ++sequence) {
                const MonData::StoredRecord record = make_string_record(
                    MonData::MonitorKey{
                        22U,
                        static_cast<std::uint32_t>(thread_index),
                        static_cast<std::uint32_t>(sequence),
                        2U},
                    "v2-value-" + std::to_string(sequence),
                    "v2-description",
                    timestamp_for(20'000U + sequence));

                if (!v2->send(record).success()) {
                    send_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::thread& sender : senders) {
        sender.join();
    }

    const std::vector<Wire::UdpReceiveResult> results =
        receiver_future.get();

    assert(send_failures.load(std::memory_order_relaxed) == 0U);
    assert(results.size() == expected_count);

    std::size_t v1_count = 0U;
    std::size_t v2_count = 0U;
    std::set<MonData::MonitorKey> seen_keys;

    for (const Wire::UdpReceiveResult& result : results) {
        assert_successful_receive(result);
        const MonData::MonitorKey& key = result._record->_data._key;
        assert(seen_keys.insert(key).second);

        if (key._mid == 11U) {
            ++v1_count;
            assert(result._record->_version == Wire::WireVersion::V1);
            assert(result._bytes_received ==
                   Wire::V1_NUMERIC_DATAGRAM_SIZE);
        } else {
            assert(key._mid == 22U);
            ++v2_count;
            assert(result._record->_version == Wire::WireVersion::V2);
            assert(string_value(result).find("v2-value-") == 0U);
        }
    }

    assert(v1_count == expected_per_version);
    assert(v2_count == expected_per_version);
    assert(seen_keys.size() == expected_count);
}

struct BoundaryExpectation final {
    Wire::WireVersion _version;
    std::size_t _encoded_size;
};

struct BoundaryJob final {
    std::shared_ptr<Wire::UdpPublisher> _publisher;
    MonData::StoredRecord _record;
};

/*
 * 案例 4：不同大小的数据报并发发送后仍保持完整边界。
 *
 * 测试功能：
 * 使用 26、19、82 和变长/最大 1200 字节等明显不同的包长，验证接收端每次
 * receive_one() 恰好得到一次 sendto() 的完整结果，没有拼包、拆包或截断。
 *
 * 测试步骤：
 * 1. 构造 V1 数值、V1 空字符串、V1 最大字符串；
 * 2. 构造 V2 数值、V2 空字符串和恰好 1200 字节的 V2 字符串；
 * 3. 每条记录由独立线程同时发送；
 * 4. 按 Key 比较实际接收长度与 encode() 的预期长度。
 */
void test_concurrent_send_preserves_datagram_boundaries() {
    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());
    enlarge_receive_buffer(receiver);

    auto v1 = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V1});
    auto v2 = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V2});
    assert(v1->ready());
    assert(v2->ready());

    const MonData::MonitorTimestamp timestamp = timestamp_for(30'000U);
    std::vector<BoundaryJob> jobs;
    jobs.push_back(BoundaryJob{
        v1,
        make_numeric_record({31U, 1U, 1U, 1U}, 1U, 0U,
                            "ignored", timestamp)});
    jobs.push_back(BoundaryJob{
        v1,
        make_string_record({31U, 1U, 2U, 1U}, "", "ignored",
                           timestamp)});
    jobs.push_back(BoundaryJob{
        v1,
        make_string_record({31U, 1U, 3U, 1U},
                           std::string(63U, 'X'), "ignored",
                           timestamp)});
    jobs.push_back(BoundaryJob{
        v2,
        make_numeric_record({32U, 2U, 1U, 2U}, 2U, 2U,
                            "v2-numeric-description", timestamp)});
    jobs.push_back(BoundaryJob{
        v2,
        make_string_record({32U, 2U, 2U, 2U}, "",
                           "v2-empty-string", timestamp)});

    const std::string max_description(10U, 'D');
    const std::size_t max_value_size =
        Wire::V2_MAX_DATAGRAM_SIZE - Wire::V2_HEADER_SIZE -
        max_description.size();
    jobs.push_back(BoundaryJob{
        v2,
        make_string_record({32U, 2U, 3U, 2U},
                           std::string(max_value_size, 'M'),
                           max_description,
                           timestamp)});

    std::map<MonData::MonitorKey, BoundaryExpectation> expected;
    for (const BoundaryJob& job : jobs) {
        const Wire::WireVersion version =
            job._publisher->version();
        const Wire::EncodeResult encoded =
            Wire::encode(job._record, version);
        assert(encoded._status == Wire::WireStatus::SUCCESS);
        expected.emplace(
            job._record._data._key,
            BoundaryExpectation{version, encoded._bytes.size()});
    }

    assert(expected.at({31U, 1U, 1U, 1U})._encoded_size ==
           Wire::V1_NUMERIC_DATAGRAM_SIZE);
    assert(expected.at({31U, 1U, 2U, 1U})._encoded_size == 19U);
    assert(expected.at({31U, 1U, 3U, 1U})._encoded_size ==
           Wire::V1_MAX_DATAGRAM_SIZE);
    assert(expected.at({32U, 2U, 3U, 2U})._encoded_size ==
           Wire::V2_MAX_DATAGRAM_SIZE);

    auto receiver_future = std::async(
        std::launch::async,
        [&receiver, count = jobs.size()] {
            return receive_exact(receiver, count, 5s);
        });

    StartGate gate(jobs.size());
    std::atomic<std::size_t> send_failures{0U};
    std::vector<std::thread> senders;
    senders.reserve(jobs.size());

    for (const BoundaryJob& job : jobs) {
        senders.emplace_back([&, job] {
            gate.arrive_and_wait();
            if (!job._publisher->send(job._record).success()) {
                send_failures.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        });
    }

    for (std::thread& sender : senders) {
        sender.join();
    }

    const std::vector<Wire::UdpReceiveResult> results =
        receiver_future.get();

    assert(send_failures.load(std::memory_order_relaxed) == 0U);
    assert(results.size() == jobs.size());

    std::set<MonData::MonitorKey> seen_keys;
    for (const Wire::UdpReceiveResult& result : results) {
        assert_successful_receive(result);
        const MonData::MonitorKey& key = result._record->_data._key;
        const auto found = expected.find(key);
        assert(found != expected.end());
        assert(seen_keys.insert(key).second);
        assert(result._record->_version == found->second._version);
        assert(result._bytes_received == found->second._encoded_size);
    }

    assert(seen_keys.size() == expected.size());
}

/*
 * 控制生命周期测试中的回调时序。
 * 所有字段都只在 _mutex 保护下访问，确保测试自身不会产生 TSan 报告。
 */
struct LifetimeState final {
    std::mutex _mutex;
    std::condition_variable _condition;
    bool _entered{false};
    bool _allow_send{false};
    bool _send_completed{false};
    bool _allow_return{false};
    std::optional<Wire::UdpPublishResult> _send_result;
};

template <typename Predicate>
bool wait_for_lifetime_state(
    const std::shared_ptr<LifetimeState>& state,
    Predicate predicate) {
    std::unique_lock<std::mutex> lock(state->_mutex);
    return state->_condition.wait_for(lock, 2s, predicate);
}

/*
 * 案例 5：在途 Engine Publisher 回调持有 UDP socket 生命周期。
 *
 * 测试功能：
 * Reporter 会在锁内复制 std::function，再解锁调用。该局部函数副本必须继续
 * 持有 lambda 捕获的 shared_ptr<UdpPublisher>，保证注销回调和释放外部引用
 * 不会让 UdpPublisher 在 send() 前或 send() 中析构。
 *
 * 测试步骤：
 * 1. 注册捕获 shared_ptr 的 Publisher，并释放测试线程自己的强引用；
 * 2. 上报线程进入回调后暂停在 send() 之前；
 * 3. 主线程注销 Publisher，确认 weak_ptr 仍未过期；
 * 4. 允许 send() 完成但暂不让回调返回，再次确认对象仍存活；
 * 5. 验证数据报完整，然后允许回调返回；
 * 6. 上报线程结束后确认 weak_ptr 过期，证明对象此时才析构。
 */
void test_inflight_callback_keeps_publisher_alive() {
    Wire::UdpReceiver receiver(
        Wire::UdpReceiverConfig{"127.0.0.1", 0U});
    assert(receiver.ready());

    Engine engine(MonConfig{
        "m6-stage11-lifetime", 9000U, 1U});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    auto publisher = std::make_shared<Wire::UdpPublisher>(
        Wire::UdpPublisherConfig{
            {"127.0.0.1", receiver.bound_port()},
            Wire::WireVersion::V2});
    assert(publisher->ready());

    std::weak_ptr<Wire::UdpPublisher> weak_publisher = publisher;
    auto state = std::make_shared<LifetimeState>();

    const bool registered = engine.set_publisher(
        [publisher, state](MonData::StoredRecord record) {
            {
                std::unique_lock<std::mutex> lock(state->_mutex);
                state->_entered = true;
                state->_condition.notify_all();
                const bool allowed = state->_condition.wait_for(
                    lock, 2s, [&] {
                        return state->_allow_send;
                    });
                if (!allowed) {
                    return;
                }
            }

            const Wire::UdpPublishResult send_result =
                publisher->send(record);

            {
                std::unique_lock<std::mutex> lock(state->_mutex);
                state->_send_result = send_result;
                state->_send_completed = true;
                state->_condition.notify_all();
                (void)state->_condition.wait_for(
                    lock, 2s, [&] {
                        return state->_allow_return;
                    });
            }
        });
    assert(registered);

    /*
     * 此后 Engine/Reporter 中的回调是唯一初始强引用持有者。
     */
    publisher.reset();
    assert(!weak_publisher.expired());

    std::optional<MonData::UpdateResult> update_result;
    std::thread updater([&] {
        update_result = engine.report_count(
            MonData::MonitorKey{40U, 1U, 1U, 1U},
            123U,
            "inflight-lifetime");
    });

    assert(wait_for_lifetime_state(state, [&] {
        return state->_entered;
    }));

    /*
     * Reporter 已经复制出本次调用的 Publisher 快照。注销只能移除未来调用
     * 使用的 Publisher，不能销毁正在执行的快照。
     */
    assert(engine.set_publisher({}));
    assert(!weak_publisher.expired());

    {
        std::lock_guard<std::mutex> lock(state->_mutex);
        state->_allow_send = true;
    }
    state->_condition.notify_all();

    assert(wait_for_lifetime_state(state, [&] {
        return state->_send_completed;
    }));

    {
        std::lock_guard<std::mutex> lock(state->_mutex);
        assert(state->_send_result.has_value());
        assert(state->_send_result->success());
    }
    assert(!weak_publisher.expired());

    const std::vector<Wire::UdpReceiveResult> received =
        receive_exact(receiver, 1U, 2s);
    assert(received.size() == 1U);
    assert_successful_receive(received.front());
    assert(received.front()._record->_version == Wire::WireVersion::V2);
    assert(numeric_value(received.front())._value == 123U);

    {
        std::lock_guard<std::mutex> lock(state->_mutex);
        state->_allow_return = true;
    }
    state->_condition.notify_all();

    updater.join();
    assert(update_result.has_value());
    assert(update_result->_status == MonData::UpdateStatus::INSERTED);
    assert(weak_publisher.expired());
}

} // namespace

int main() {
    test_v1_publisher_concurrent_send();
    test_v2_publisher_concurrent_send();
    test_v1_and_v2_publishers_share_receiver();
    test_concurrent_send_preserves_datagram_boundaries();
    test_inflight_callback_keeps_publisher_alive();

    std::cout << "M6_STAGE11_V1_CONCURRENT=PASS\n";
    std::cout << "M6_STAGE11_V2_CONCURRENT=PASS\n";
    std::cout << "M6_STAGE11_DUAL_VERSION=PASS\n";
    std::cout << "M6_STAGE11_DATAGRAM_BOUNDARY=PASS\n";
    std::cout << "M6_STAGE11_SHARED_LIFETIME=PASS\n";
    return 0;
}
