#include "engine.h"
#include "monitor_data.h"
#include "monitor_wire.h"
#include "timer_types.h"
#include "udp_publisher.h"
#include "udp_receiver.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace std::chrono_literals;
namespace {
/*
 * Demo 只使用一个模块。
 *
 * Key 的四个字段依次为：
 * mid、level、fid、eid。
 */
constexpr std::uint32_t DEMO_MID = 0x100U;

constexpr MonData::MonitorKey COUNT_KEY{DEMO_MID, 1U, 0U, 0U};

constexpr MonData::MonitorKey ERROR_KEY{DEMO_MID, 2U, 0U, 1U};

constexpr MonData::MonitorKey STRING_KEY{DEMO_MID, 1U, 0U, 2U};
/*
 * 故意包含重复值，用于证明 M4 去重能够一直作用到 UDP 边界。
 *
 * 变化情况：
 * 10       → INSERTED
 * 10       → UNCHANGED
 * 20       → UPDATED
 * 20       → UNCHANGED
 * 30       → UPDATED
 * 0        → UPDATED
 * 0        → UNCHANGED
 *
 * 因此 7 次数值上报只产生 4 个 UDP 数据报。
 */
constexpr std::array<std::uint32_t, 7U> COUNT_SEQUENCE{10U, 10U, 20U, 20U,
                                                       30U, 0U,  0U};
constexpr std::size_t EXPECTED_REPORTS = 9U;
constexpr std::size_t EXPECTED_CHANGED = 6U;
constexpr std::size_t EXPECTED_UNCHANGED = 3U;
constexpr std::size_t EXPECTED_DATAGRAMS = 6U;
constexpr std::size_t EXPECTED_STORE_RECORDS = 3U;
constexpr std::size_t LONG_STRING_SIZE = 128U;
/*
 * DemoState 被所有回调通过 shared_ptr 捕获。
 *
 * 当前所有回调都使用同步模式：
 *
 *     MonCallback{..., false}
 *
 * 因此这些字段只由 Engine 运行线程访问，不需要 mutex。
 * 如果以后把任一回调改成 WORKER/asynchronous，就必须重新设计同步。
 */
struct DemoState final {
  std::size_t reports{0U};
  std::size_t changed{0U};
  std::size_t unchanged{0U};
  std::size_t invalid_updates{0U};

  std::size_t published{0U};
  std::size_t send_errors{0U};

  std::size_t received{0U};
  std::size_t receive_errors{0U};

  std::size_t sequence_index{0U};

  bool sequence_complete{false};
  bool stop_requested{false};
  bool timed_out{false};

  /*
   * 保存解码结果，Engine 停止后用于验证 V2 metadata。
   */
  std::vector<Wire::DecodedRecord> decoded_records;
};
const char *update_status_name(MonData::UpdateStatus status) noexcept {

  switch (status) {
  case MonData::UpdateStatus::INSERTED:
    return "INSERTED";

  case MonData::UpdateStatus::UPDATED:
    return "UPDATED";

  case MonData::UpdateStatus::UNCHANGED:
    return "UNCHANGED";

  case MonData::UpdateStatus::IGNORED_INITIAL_ZERO:
    return "IGNORED_INITIAL_ZERO";

  case MonData::UpdateStatus::INVALID:
    return "INVALID";
  }

  return "UNKNOWN";
}
/*
 * 统一统计一次 report_*() 的结果。
 */
void record_update_result(DemoState &state,
                          const MonData::UpdateResult &result) {

  ++state.reports;

  if (result.changed()) {
    ++state.changed;
    return;
  }

  if (result._status == MonData::UpdateStatus::UNCHANGED) {
    ++state.unchanged;
    return;
  }

  /*
   * 本 Demo 不应该出现 INVALID 或 IGNORED_INITIAL_ZERO。
   *
   * 普通零值是在已有非零值之后出现，所以应当是 UPDATED；
   * error=0 使用 report_error()，第一次零值也会强制写入。
   */
  ++state.invalid_updates;
}

/*
 * 避免多个回调重复调用 Engine::stop()。
 *
 * stop() 本身允许重复调用，但单独维护标志可以让 Demo 生命周期更清晰。
 */
void request_stop(Engine &engine, DemoState &state) {

  if (state.stop_requested) {
    return;
  }

  state.stop_requested = true;
  engine.stop();
}

/*
 * 正常停止条件：
 *
 * 1. 7 次周期数值上报已经执行完；
 * 2. 6 个变化记录已经全部从 UDP Receiver 收到。
 *
 * 不能仅根据 sequence_complete 停止，否则最后一个 UDP 包可能还在
 * socket 接收队列中，尚未被 AIO 回调读取。
 */
void stop_if_complete(Engine &engine, DemoState &state) {

  if (state.sequence_complete && state.received == EXPECTED_DATAGRAMS) {
    request_stop(engine, state);
  }
}
/*
 * 打印一条已解码的 V2 记录。
 *
 * 字符串只打印长度，不输出完整 128 字节内容，避免日志过长。
 */
void print_decoded_record(const Wire::DecodedRecord &record) {

  const MonData::MonitorKey &key = record._data._key;

  std::cout << "[recv]" << " version=" << static_cast<unsigned>(record._version)
            << " mid=" << key._mid << " level=" << key._level
            << " fid=" << key._fid << " eid=" << key._eid << " description=\""
            << record._data._description << "\"";

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);

  if (numeric != nullptr) {
    std::cout << " value=" << numeric->_value << " state=" << numeric->_state;
  } else {
    const auto *text = std::get_if<std::string>(&record._data._value);

    if (text != nullptr) {
      std::cout << " string_length=" << text->size();
    }
  }

  std::cout << " changed_at="
            << (record._changed_at.has_value() ? "present" : "missing") << '\n'
            << std::flush;
}
/*
 * AIO fd 可读时，一次性排空 Receiver。
 *
 * UdpReceiver 使用非阻塞 socket。
 * WOULD_BLOCK 表示本轮队列已经排空，是正常结束条件。
 */
int drain_receiver(Engine &engine,
                   const std::shared_ptr<Wire::UdpReceiver> &receiver,
                   const std::shared_ptr<DemoState> &state) {

  while (true) {
    Wire::UdpReceiveResult result = receiver->receive_one();

    if (result._status == Wire::UdpReceiveStatus::WOULD_BLOCK) {
      return 0;
    }

    if (!result.success() || !result._record.has_value()) {

      ++state->receive_errors;

      std::cerr << "[recv] failed"
                << " receive_status=" << static_cast<int>(result._status)
                << " wire_status=" << static_cast<int>(result._wire_status)
                << " errno=" << result._system_error << '\n';

      /*
       * Demo 中任何协议或接收错误都视为失败，直接请求停止。
       */
      request_stop(engine, *state);
      return 0;
    }

    ++state->received;

    state->decoded_records.push_back(std::move(*result._record));

    print_decoded_record(state->decoded_records.back());

    stop_if_complete(engine, *state);
  }
}

const Wire::DecodedRecord *find_latest_decoded(const DemoState &state,
                                               const MonData::MonitorKey &key) {

  for (auto iterator = state.decoded_records.rbegin();
       iterator != state.decoded_records.rend(); ++iterator) {

    if (iterator->_data._key == key) {
      return &*iterator;
    }
  }

  return nullptr;
}

/*
 * 检查一条 StoredRecord 是否包含指定 numeric value/state。
 */
bool stored_numeric_equals(const std::optional<MonData::StoredRecord> &record,
                           std::uint32_t value, std::uint32_t state) {

  if (!record.has_value()) {
    return false;
  }

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record->_data._value);

  return numeric != nullptr && numeric->_value == value &&
         numeric->_state == state;
}

/*
 * 检查一条解码记录是否包含指定 numeric value/state。
 */
bool decoded_numeric_equals(const Wire::DecodedRecord *record,
                            std::uint32_t value, std::uint32_t state) {

  if (record == nullptr) {
    return false;
  }

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record->_data._value);

  return numeric != nullptr && numeric->_value == value &&
         numeric->_state == state;
}

/*
 * 输出稳定的 PASS/FAIL 验收行。
 */
void print_check(const char *name, bool passed) {

  std::cout << name << '=' << (passed ? "PASS" : "FAIL") << '\n';
}
} // namespace

int main() {
  /*
   * 步骤 1：创建共享状态。
   *
   * shared_ptr 保证所有 Timer/AIO/Publisher 回调执行期间状态有效。
   */
  auto state = std::make_shared<DemoState>();

  /*
   * 步骤 2：先创建 Receiver。
   *
   * 端口传 0，让系统选择空闲端口，避免固定 9100 与其他测试冲突。
   */
  auto receiver = std::make_shared<Wire::UdpReceiver>(
      Wire::UdpReceiverConfig{"127.0.0.1", 0U});

  if (!receiver->ready()) {
    std::cerr << "[demo_pub] receiver setup failed"
              << " status=" << static_cast<int>(receiver->setup_status())
              << '\n';

    return 1;
  }

  /*
   * 步骤 3：创建并初始化 Engine。
   *
   * MonConfig::_port 在当前纯 M6 Demo 中不用于绑定 CLI，
   * 但仍给出一个明确值以保持配置语义清楚。
   */
  Engine engine(MonConfig{"demo_pub", 9101U, 1U});

  const ENGINESTATE init_result = engine.init();

  if (init_result != ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[demo_pub] engine init failed"
              << " status=" << static_cast<int>(init_result) << '\n';

    return 1;
  }

  /*
   * 步骤 4：把 Receiver fd 注册到 Engine AIO。
   *
   * Receiver 仍拥有并负责关闭 fd；
   * Engine 只监听该 fd，不接管所有权。
   */
  const std::optional<AioHandle> receiver_handle = engine.add_aio(
      receiver->fd(), MonCallback{"demo-pub-receiver",
                                  [&engine, receiver, state]() -> int {
                                    return drain_receiver(engine, receiver,
                                                          state);
                                  },
                                  false});

  if (!receiver_handle.has_value()) {
    std::cerr << "[demo_pub] failed to register receiver AIO\n";

    return 1;
  }

  /*
   * 步骤 5：创建 V2 Publisher。
   *
   * 目标端口直接使用 Receiver 实际绑定端口。
   */
  auto publisher =
      std::make_shared<Wire::UdpPublisher>(Wire::UdpPublisherConfig{
          {"127.0.0.1", receiver->bound_port()}, Wire::WireVersion::V2});

  if (!publisher->ready()) {
    std::cerr << "[demo_pub] publisher setup failed"
              << " status=" << static_cast<int>(publisher->setup_status())
              << '\n';

    return 1;
  }

  /*
   * 步骤 6：将 UDP Publisher 安装到 M5 Publisher seam。
   *
   * MonitorReporter 只会为 INSERTED/UPDATED 调用该回调；
   * UNCHANGED 不会进入这里。
   */
  const bool publisher_installed = engine.set_publisher(
      [&engine, publisher, state](MonData::StoredRecord record) {
        const Wire::UdpPublishResult result = publisher->send(record);

        if (result.success()) {
          ++state->published;

          std::cout << "[publish] bytes=" << result._bytes_sent << '\n'
                    << std::flush;

          return;
        }

        ++state->send_errors;

        std::cerr << "[publish] failed"
                  << " status=" << static_cast<int>(result._status)
                  << " wire_status=" << static_cast<int>(result._wire_status)
                  << " errno=" << result._system_error << '\n';

        /*
         * Publisher 回调执行时 Engine 不持有 control mutex，
         * 因此这里允许重新进入 engine.stop()。
         */
        request_stop(engine, *state);
      });

  if (!publisher_installed) {
    std::cerr << "[demo_pub] failed to install publisher\n";

    return 1;
  }

  /*
   * 步骤 7：注册 error=0 一次性 Timer。
   *
   * report_error() 使用 force=true，因此第一次值为 0 也必须 INSERTED
   * 并产生 UDP 数据报。
   */
  const auto error_timer = engine.set_timer(
      MonCallback{"demo-pub-error",
                  [&engine, state]() -> int {
                    const MonData::UpdateResult result =
                        engine.report_error(ERROR_KEY, 0U, "link down");

                    record_update_result(*state, result);

                    std::cout << "[report] error value=0" << " status="
                              << update_status_name(result._status) << '\n'
                              << std::flush;

                    return 0;
                  },
                  false},
      TimerFlags::ONCE, 100ms);

  if (!error_timer.has_value()) {
    std::cerr << "[demo_pub] failed to register error timer\n";

    return 1;
  }

  /*
   * 步骤 8：注册 V2 长字符串一次性 Timer。
   *
   * 128 字节超过 V1 的 63 字节上限，
   * 但远小于 V2 的 1200 字节数据报限制。
   */
  const auto string_timer = engine.set_timer(
      MonCallback{"demo-pub-string",
                  [&engine, state]() -> int {
                    const std::string value(LONG_STRING_SIZE, 'L');

                    const MonData::UpdateResult result =
                        engine.report_string(STRING_KEY, value, "boot phase");

                    record_update_result(*state, result);

                    std::cout
                        << "[report] string" << " length=" << value.size()
                        << " status=" << update_status_name(result._status)
                        << '\n'
                        << std::flush;

                    return 0;
                  },
                  false},
      TimerFlags::ONCE, 150ms);

  if (!string_timer.has_value()) {
    std::cerr << "[demo_pub] failed to register string timer\n";

    return 1;
  }

  /*
   * 步骤 9：注册周期数值 Timer。
   *
   * 回调完成 7 次上报后不再产生新数据，但 Timer 会继续存在，
   * 直到 Engine 停止时统一清理。
   */
  const auto count_timer = engine.set_timer(
      MonCallback{
          "demo-pub-count",
          [&engine, state]() -> int {
            if (state->sequence_complete) {
              return 0;
            }

            const std::uint32_t value = COUNT_SEQUENCE[state->sequence_index];

            ++state->sequence_index;

            const MonData::UpdateResult result =
                engine.report_count(COUNT_KEY, value, "cpu temperature");

            record_update_result(*state, result);

            std::cout << "[report] count" << " value=" << value
                      << " status=" << update_status_name(result._status)
                      << '\n'
                      << std::flush;

            if (state->sequence_index == COUNT_SEQUENCE.size()) {

              state->sequence_complete = true;

              /*
               * 最后一次上报是重复 0，不产生新 UDP 包。
               * 如果此前 6 个数据报都已收到，可以立即停止。
               */
              stop_if_complete(engine, *state);
            }

            return 0;
          },
          false},
      TimerFlags::RECURRING, 200ms);

  if (!count_timer.has_value()) {
    std::cerr << "[demo_pub] failed to register count timer\n";

    return 1;
  }

  /*
   * 步骤 10：注册 watchdog。
   *
   * 正常 Demo 在约 1.4 秒结束。
   * 5 秒仍未完成说明 UDP、Timer 或 AIO 链路存在问题。
   */
  const auto watchdog_timer =
      engine.set_timer(MonCallback{"demo-pub-watchdog",
                                   [&engine, state]() -> int {
                                     state->timed_out = true;

                                     std::cerr
                                         << "[demo_pub] watchdog timeout\n";

                                     request_stop(engine, *state);

                                     return 0;
                                   },
                                   false},
                       TimerFlags::ONCE, 5s);

  if (!watchdog_timer.has_value()) {
    std::cerr << "[demo_pub] failed to register watchdog\n";

    return 1;
  }

  /*
   * 步骤 11：打印启动信息。
   */
  std::cout << "pipeline: report" << " -> dedup" << " -> V2 encode"
            << " -> UDP:" << receiver->bound_port() << " -> AIO receive"
            << " -> decode\n"
            << std::flush;

  /*
   * 步骤 12：阻塞运行 Engine。
   *
   * 正常停止由 stop_if_complete() 触发；
   * 异常停止由 Publisher、Receiver 或 watchdog 触发。
   */
  const ENGINESTATE run_result = engine.run();

  /*
   * 步骤 13：Engine 停止后验证 Store。
   *
   * 当前 Engine 设计允许 STOPPED 状态继续读取最后记录。
   */
  const auto count_stored = engine.find_data(COUNT_KEY);

  const auto error_stored = engine.find_data(ERROR_KEY);

  const auto string_stored = engine.find_data(STRING_KEY);

  const std::vector<MonData::StoredRecord> store_snapshot = engine.query_data();

  const bool count_store_ok = stored_numeric_equals(count_stored, 0U, 0U);

  const bool error_store_ok = stored_numeric_equals(error_stored, 0U, 2U);

  bool string_store_ok = false;

  if (string_stored.has_value()) {
    const auto *value = std::get_if<std::string>(&string_stored->_data._value);

    string_store_ok = value != nullptr && value->size() == LONG_STRING_SIZE &&
                      string_stored->_data._description == "boot phase";
  }

  /*
   * 步骤 14：验证接收到的 V2 数据。
   */
  const Wire::DecodedRecord *final_count =
      find_latest_decoded(*state, COUNT_KEY);

  const Wire::DecodedRecord *error_record =
      find_latest_decoded(*state, ERROR_KEY);

  const Wire::DecodedRecord *string_record =
      find_latest_decoded(*state, STRING_KEY);

  const bool error_wire_ok = decoded_numeric_equals(error_record, 0U, 2U);

  bool string_wire_ok = false;

  if (string_record != nullptr) {
    const auto *value = std::get_if<std::string>(&string_record->_data._value);

    string_wire_ok = value != nullptr && value->size() == LONG_STRING_SIZE &&
                     string_record->_data._description == "boot phase" &&
                     string_record->_changed_at.has_value();
  }

  /*
   * 所有接收记录都必须明确是 V2，并携带 description/timestamp。
   */
  const bool metadata_ok =
      std::all_of(state->decoded_records.begin(), state->decoded_records.end(),
                  [](const Wire::DecodedRecord &record) {
                    return record._version == Wire::WireVersion::V2 &&
                           !record._data._description.empty() &&
                           record._changed_at.has_value();
                  });

  /*
   * COUNT_KEY 应收到 10、20、30、0 四个变化包。
   */
  const std::size_t count_datagrams = static_cast<std::size_t>(std::count_if(
      state->decoded_records.begin(), state->decoded_records.end(),
      [](const Wire::DecodedRecord &record) {
        return record._data._key == COUNT_KEY;
      }));

  const bool reports_ok = state->reports == EXPECTED_REPORTS;

  const bool dedup_ok =
      state->changed == EXPECTED_CHANGED &&
      state->unchanged == EXPECTED_UNCHANGED && state->invalid_updates == 0U &&
      state->published == EXPECTED_DATAGRAMS &&
      state->received == EXPECTED_DATAGRAMS && count_datagrams == 4U &&
      decoded_numeric_equals(final_count, 0U, 0U);

  const bool transport_ok =
      state->send_errors == 0U && state->receive_errors == 0U;

  const bool store_ok = store_snapshot.size() == EXPECTED_STORE_RECORDS &&
                        count_store_ok && error_store_ok && string_store_ok;

  const bool lifecycle_ok = run_result == ENGINESTATE::SUCCESSFUL &&
                            engine.get_phase() == EnginePhase::STOPPED &&
                            state->sequence_complete && !state->timed_out;

  /*
   * 步骤 15：计算最终结果。
   */
  const bool overall = reports_ok && dedup_ok && transport_ok && store_ok &&
                       lifecycle_ok && error_wire_ok && string_wire_ok &&
                       metadata_ok;

  /*
   * 步骤 16：输出稳定的机器可读验收标记。
   */
  std::cout << "DEMO_PUB_REPORTS=" << state->reports << '\n';

  std::cout << "DEMO_PUB_CHANGED=" << state->changed << '\n';

  std::cout << "DEMO_PUB_UNCHANGED=" << state->unchanged << '\n';

  std::cout << "DEMO_PUB_DATAGRAMS=" << state->received << '\n';

  std::cout << "DEMO_PUB_STORE_RECORDS=" << store_snapshot.size() << '\n';

  print_check("DEMO_PUB_DEDUP", dedup_ok);

  print_check("DEMO_PUB_ERROR_ZERO", error_store_ok && error_wire_ok);

  print_check("DEMO_PUB_V2_LONG_STRING", string_store_ok && string_wire_ok);

  print_check("DEMO_PUB_V2_METADATA", metadata_ok);

  print_check("DEMO_PUB_OVERALL", overall);

  /*
   * 步骤 17：成功返回 0，任一验收失败返回 1。
   */
  return overall ? 0 : 1;
}
