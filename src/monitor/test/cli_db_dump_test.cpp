#include "cli_commands.h"
#include "engine.h"
#include "monitor_data.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace TLSSMON;

constexpr std::int64_t kNanosecondsPerSecond =
    INT64_C(1'000'000'000);

void initialize(Engine& engine)
{
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::READY);
}

/*
 * 测试侧独立计算预期时间戳，验证 db_dump 输出记录中的 changed_at。
 */
std::string expected_timestamp(
    MonData::MonitorTimestamp timestamp)
{
    const std::int64_t total_nanoseconds =
        static_cast<std::int64_t>(
            timestamp.time_since_epoch().count());

    std::int64_t seconds =
        total_nanoseconds / kNanosecondsPerSecond;

    std::int64_t nanoseconds =
        total_nanoseconds % kNanosecondsPerSecond;

    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += kNanosecondsPerSecond;
    }

    std::string fraction = std::to_string(nanoseconds);
    fraction.insert(0U, 9U - fraction.size(), '0');

    return std::to_string(seconds) + '.' + fraction;
}

void assert_has_suffix(
    const std::string& value,
    const std::string& suffix)
{
    assert(value.size() >= suffix.size());
    assert(
        value.compare(
            value.size() - suffix.size(),
            suffix.size(),
            suffix)
        == 0);
}

/*
 * db_dump 注册成功，并携带固定帮助文本。
 */
void test_register_db_dump_command()
{
    Engine engine{MonConfig{"db-dump-register", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const std::vector<CliCommandInfo> commands =
        registry.commands();

    assert(commands.size() == 1U);
    assert(commands[0]._name == "db_dump");
    assert(commands[0]._help == "dump monitor records");
}

/*
 * 重复注册不能覆盖原来的 db_dump Handler。
 */
void test_duplicate_db_dump_is_rejected()
{
    Engine engine{MonConfig{"db-dump-duplicate", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::DUPLICATE_COMMAND);

    assert(registry.commands().size() == 1U);
}

/*
 * 空 Store 仍返回明确的记录数量。
 */
void test_empty_store()
{
    Engine engine{MonConfig{"db-dump-empty", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "0 entry\n");
}

/*
 * 数值记录输出完整 Key、value、state、description 和时间戳。
 */
void test_numeric_record()
{
    Engine engine{MonConfig{"db-dump-numeric", 0U, 1U}};
    initialize(engine);

    const MonData::UpdateResult update =
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{7U, 2U, 8U, 9U},
                "temperature",
                MonData::NumericValue{123U, 2U}
            });

    assert(update.changed());
    assert(update._record.has_value());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    const std::string expected =
        "mid=7 lvl=sstop fid=8 eid=9 "
        "num=123 state=2 "
        "desc=\"temperature\" "
        "changed_at="
        + expected_timestamp(update._record->_changed_at)
        + "\n1 entry\n";

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == expected);
}

/*
 * 字符串记录使用 str 字段，不能误输出 num/state。
 */
void test_string_record()
{
    Engine engine{MonConfig{"db-dump-string", 0U, 1U}};
    initialize(engine);

    const MonData::UpdateResult update =
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{1U, 3U, 4U, 5U},
                "service status",
                std::string{"running"}
            });

    assert(update.changed());
    assert(update._record.has_value());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    const std::string expected =
        "mid=1 lvl=estop fid=4 eid=5 "
        "str=\"running\" "
        "desc=\"service status\" "
        "changed_at="
        + expected_timestamp(update._record->_changed_at)
        + "\n1 entry\n";

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == expected);
}

/*
 * 空字符串是合法值，必须保留为 str=""。
 */
void test_empty_string_record()
{
    Engine engine{MonConfig{"db-dump-empty-string", 0U, 1U}};
    initialize(engine);

    assert(
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{1U, 1U, 1U, 1U},
                "",
                std::string{}
            }).changed());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output.find("str=\"\"") != std::string::npos);
    assert(result._output.find("desc=\"\"") != std::string::npos);
}

/*
 * UTF-8 字节必须完整保留，不能按旧协议长度截断。
 */
void test_utf8_string_and_description()
{
    Engine engine{MonConfig{"db-dump-utf8", 0U, 1U}};
    initialize(engine);

    assert(
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{2U, 2U, 2U, 2U},
                "服务状态",
                std::string{"运行中"}
            }).changed());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output.find("str=\"运行中\"") != std::string::npos);
    assert(result._output.find("desc=\"服务状态\"") != std::string::npos);
}

/*
 * NUL、换行、反斜杠、引号和控制字符必须可逆地转义。
 */
void test_string_escaping()
{
    Engine engine{MonConfig{"db-dump-escape", 0U, 1U}};
    initialize(engine);

    const std::string value{"A\0B\n\\\"", 6U};

    std::string description{"D\t\r"};
    description.push_back('\x01');
    description.push_back('\x7F');

    assert(
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{3U, 3U, 3U, 3U},
                description,
                value
            }).changed());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    assert(result._status == CliDispatchStatus::SUCCESS);

    assert(
        result._output.find("str=\"A\\x00B\\n\\\\\\\"\"")
        != std::string::npos);

    assert(
        result._output.find("desc=\"D\\t\\r\\x01\\x7F\"")
        != std::string::npos);
}

/*
 * 输出顺序保持 mid -> level -> fid -> eid。
 */
void test_record_order()
{
    Engine engine{MonConfig{"db-dump-order", 0U, 1U}};
    initialize(engine);

    const MonData::MonitorData records[]{
        {
            MonData::MonitorKey{2U, 0U, 0U, 0U},
            "third",
            MonData::NumericValue{3U, 0U}
        },
        {
            MonData::MonitorKey{1U, 2U, 0U, 0U},
            "second",
            MonData::NumericValue{2U, 0U}
        },
        {
            MonData::MonitorKey{1U, 1U, 9U, 9U},
            "first",
            MonData::NumericValue{1U, 0U}
        }
    };

    for (const MonData::MonitorData& record : records) {
        assert(engine.update_data(record).changed());
    }

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    const std::size_t first =
        result._output.find("mid=1 lvl=warn fid=9 eid=9");
    const std::size_t second =
        result._output.find("mid=1 lvl=sstop fid=0 eid=0");
    const std::size_t third =
        result._output.find("mid=2 lvl=info fid=0 eid=0");

    assert(first != std::string::npos);
    assert(second != std::string::npos);
    assert(third != std::string::npos);
    assert(first < second);
    assert(second < third);

    assert_has_suffix(result._output, "3 entries\n");
}

/*
 * 未知 level 不能让 db_dump 失败，必须显示为 "?"。
 * description 回填属于后续阶段，因此这里仍保持 Store 中的空字符串。
 */
void test_unknown_level_uses_question_mark()
{
    Engine engine{MonConfig{"db-dump-no-m8", 0U, 1U}};
    initialize(engine);

    assert(
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{10U, 99U, 20U, 30U},
                "",
                MonData::NumericValue{7U, 0U}
            }).changed());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output.find("mid=10 lvl=? fid=20 eid=30")
        != std::string::npos);
    assert(result._output.find("lvl=99") == std::string::npos);
    assert(result._output.find("desc=\"\"") != std::string::npos);
}

/*
 * changed_at 的小数部分必须固定为 9 位纳秒。
 */
void test_timestamp_has_nine_nanosecond_digits()
{
    Engine engine{MonConfig{"db-dump-timestamp", 0U, 1U}};
    initialize(engine);

    assert(
        engine.update_data(
            MonData::MonitorData{
                MonData::MonitorKey{1U, 1U, 1U, 1U},
                "timestamp",
                MonData::NumericValue{1U, 0U}
            }).changed());

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump");

    const std::string marker{"changed_at="};
    const std::size_t begin = result._output.find(marker);

    assert(begin != std::string::npos);

    const std::size_t timestamp_begin = begin + marker.size();
    const std::size_t line_end =
        result._output.find('\n', timestamp_begin);

    assert(line_end != std::string::npos);

    const std::string timestamp =
        result._output.substr(
            timestamp_begin,
            line_end - timestamp_begin);

    const std::size_t dot = timestamp.find('.');

    assert(dot != std::string::npos);
    assert(timestamp.size() - dot - 1U == 9U);

    for (std::size_t index = dot + 1U;
         index < timestamp.size();
         ++index) {
        assert(timestamp[index] >= '0');
        assert(timestamp[index] <= '9');
    }
}

/*
 * db_dump 与写入并发时不能产生数据竞争、死锁或内部悬空引用。
 */
void test_dump_and_updates_can_run_concurrently()
{
    Engine engine{MonConfig{"db-dump-concurrent", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    constexpr std::size_t record_count = 100U;
    constexpr std::size_t dump_count = 200U;

    std::atomic<bool> start{false};

    std::thread writer{
        [&engine, &start] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0U;
                 index < record_count;
                 ++index) {
                const MonData::UpdateResult result =
                    engine.update_data(
                        MonData::MonitorData{
                            MonData::MonitorKey{
                                1U,
                                1U,
                                static_cast<std::uint32_t>(index),
                                1U
                            },
                            "concurrent",
                            MonData::NumericValue{
                                static_cast<std::uint32_t>(index + 1U),
                                0U
                            }
                        });

                assert(result.changed());
            }
        }};

    std::thread dumper{
        [&registry, &start] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0U;
                 index < dump_count;
                 ++index) {
                const CliDispatchResult result =
                    registry.dispatch("db_dump");

                assert(result._status == CliDispatchStatus::SUCCESS);

                /*
                 * 0 和 1 使用单数 entry，2 及以上使用复数 entries。
                 * 并发查询得到的快照大小不固定，因此两种合法后缀都接受。
                 */
                const bool has_singular_count =
                    result._output.find(" entry\n") != std::string::npos;
                const bool has_plural_count =
                    result._output.find(" entries\n") != std::string::npos;

                assert(has_singular_count || has_plural_count);
            }
        }};

    start.store(true, std::memory_order_release);

    writer.join();
    dumper.join();

    const CliDispatchResult final_result =
        registry.dispatch("db_dump");

    assert(final_result._status == CliDispatchStatus::SUCCESS);

    const std::size_t line_count =
        static_cast<std::size_t>(
            std::count(
                final_result._output.begin(),
                final_result._output.end(),
                '\n'));

    assert(line_count == record_count + 1U);
    assert_has_suffix(final_result._output, "100 entries\n");
}

/*
 * 与旧 M7 一致，db_dump 忽略额外参数。
 */
void test_db_dump_ignores_arguments()
{
    Engine engine{MonConfig{"db-dump-arguments", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("db_dump ignored argument");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "0 entry\n");
}

/*
 * 注册 help 和 db_dump 后，help 应列出 db_dump。
 */
void test_help_lists_db_dump()
{
    Engine engine{MonConfig{"db-dump-help", 0U, 1U}};
    initialize(engine);

    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    assert(
        register_db_dump_command(registry, engine)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output
        == "db_dump    dump monitor records\n"
           "help       list available commands\n");
}

} // namespace

int main()
{
    test_register_db_dump_command();
    test_duplicate_db_dump_is_rejected();
    test_empty_store();
    test_numeric_record();
    test_string_record();
    test_empty_string_record();
    test_utf8_string_and_description();
    test_string_escaping();
    test_record_order();
    test_unknown_level_uses_question_mark();
    test_timestamp_has_nine_nanosecond_digits();
    test_dump_and_updates_can_run_concurrently();
    test_db_dump_ignores_arguments();
    test_help_lists_db_dump();

    std::cout << "CLI_DB_DUMP_TEST=PASS\n";
    return 0;
}
