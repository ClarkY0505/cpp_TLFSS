#include "cli_registry.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace TLSSMON;

constexpr std::size_t kWriterCommands = 100U;
constexpr std::size_t kReaderThreads = 4U;
constexpr std::size_t kCallsPerReader = 500U;

CliHandler make_handler(std::string output = "ok\n")
{
    return [output = std::move(output)](
               const CliArguments&) {
        return output;
    };
}

/*
 * 注册后可以通过命令名执行 Handler，且参数不包含命令名。
 */
void test_register_and_dispatch()
{
    CliRegistry registry;
    CliArguments received_arguments;
    std::size_t call_count = 0U;

    const CliRegisterStatus registered =
        registry.register_command(
            "echo",
            "echo arguments",
            [&](const CliArguments& arguments) {
                ++call_count;
                received_arguments = arguments;
                return std::string{"echoed\n"};
            });

    assert(registered == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("echo first second");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "echoed\n");
    assert(call_count == 1U);
    assert(received_arguments.size() == 2U);
    assert(received_arguments[0] == "first");
    assert(received_arguments[1] == "second");
}

/*
 * 空命令名或含空白字符的命令名必须被拒绝。
 */
void test_invalid_command_names_are_rejected()
{
    CliRegistry registry;

    const std::vector<std::string> invalid_names{
        "",
        " ",
        "\t",
        "\n",
        "\r",
        "two words",
        "bad\tname",
        "bad\nname",
        "bad\rname"
    };

    for (const std::string& name : invalid_names) {
        assert(
            registry.register_command(
                name,
                "invalid",
                make_handler())
            == CliRegisterStatus::INVALID_NAME);
    }

    assert(registry.commands().empty());
}

/*
 * 空 std::function 不是合法 Handler。
 */
void test_empty_handler_is_rejected()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "empty",
            "empty handler",
            CliHandler{})
        == CliRegisterStatus::INVALID_HANDLER);

    assert(registry.commands().empty());
}

/*
 * 重复注册不能覆盖旧 Handler 和旧帮助文本。
 */
void test_duplicate_command_is_rejected()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "status",
            "first help",
            make_handler("first\n"))
        == CliRegisterStatus::SUCCESS);

    assert(
        registry.register_command(
            "status",
            "second help",
            make_handler("second\n"))
        == CliRegisterStatus::DUPLICATE_COMMAND);

    const CliDispatchResult result =
        registry.dispatch("status");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "first\n");

    const std::vector<CliCommandInfo> commands =
        registry.commands();

    assert(commands.size() == 1U);
    assert(commands[0]._help == "first help");
}

/*
 * 命令名区分大小写。
 */
void test_command_name_is_case_sensitive()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "Echo",
            "case sensitive",
            make_handler())
        == CliRegisterStatus::SUCCESS);

    assert(
        registry.dispatch("Echo")._status
        == CliDispatchStatus::SUCCESS);

    assert(
        registry.dispatch("echo")._status
        == CliDispatchStatus::UNKNOWN_COMMAND);
}

/*
 * Registry 必须拥有注册参数，不能保存调用者字符串引用。
 */
void test_registry_owns_registered_values()
{
    CliRegistry registry;

    std::string name{"owned"};
    std::string help{"owned help"};
    std::string output{"owned output\n"};

    assert(
        registry.register_command(
            name,
            help,
            [output](const CliArguments&) {
                return output;
            })
        == CliRegisterStatus::SUCCESS);

    name.clear();
    help.clear();
    output.clear();

    const CliDispatchResult dispatched =
        registry.dispatch("owned");

    assert(dispatched._status == CliDispatchStatus::SUCCESS);
    assert(dispatched._output == "owned output\n");

    const std::vector<CliCommandInfo> commands =
        registry.commands();

    assert(commands.size() == 1U);
    assert(commands[0]._name == "owned");
    assert(commands[0]._help == "owned help");
}

/*
 * 空行和纯空白行不会执行任何命令。
 */
void test_empty_input_is_rejected()
{
    CliRegistry registry;

    assert(
        registry.dispatch("")._status
        == CliDispatchStatus::EMPTY_INPUT);

    assert(
        registry.dispatch("  \t\r\n")._status
        == CliDispatchStatus::EMPTY_INPUT);
}

/*
 * 未知命令返回稳定状态和可直接发送给客户端的提示。
 */
void test_unknown_command()
{
    CliRegistry registry;

    const CliDispatchResult result =
        registry.dispatch("missing first");

    assert(result._status == CliDispatchStatus::UNKNOWN_COMMAND);
    assert(result._output == "unknown command: missing\n");
}

/*
 * 连续空格、制表符和行结束空白都作为 token 分隔符。
 */
void test_whitespace_tokenization()
{
    CliRegistry registry;
    CliArguments received;

    assert(
        registry.register_command(
            "echo",
            "echo arguments",
            [&](const CliArguments& arguments) {
                received = arguments;
                return std::string{"ok\n"};
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch(
            " \t echo   first\tsecond  third \r\n");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(received.size() == 3U);
    assert(received[0] == "first");
    assert(received[1] == "second");
    assert(received[2] == "third");
}

/*
 * 八个 token 包含命令名，因此最多允许七个参数。
 */
void test_eight_tokens_are_allowed()
{
    CliRegistry registry;
    std::size_t argument_count = 0U;

    assert(
        registry.register_command(
            "command",
            "token test",
            [&](const CliArguments& arguments) {
                argument_count = arguments.size();
                return std::string{"ok\n"};
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("command 1 2 3 4 5 6 7");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(argument_count == 7U);
}

/*
 * 第九个 token 必须拒绝，不能静默截断后调用 Handler。
 */
void test_nine_tokens_are_rejected()
{
    CliRegistry registry;
    std::size_t call_count = 0U;

    assert(
        registry.register_command(
            "command",
            "token test",
            [&](const CliArguments&) {
                ++call_count;
                return std::string{"unexpected\n"};
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("command 1 2 3 4 5 6 7 8");

    assert(result._status == CliDispatchStatus::TOO_MANY_TOKENS);
    assert(result._output == "error: too many tokens\n");
    assert(call_count == 0U);
}

/*
 * 恰好 255 字节的命令行合法。
 */
void test_255_byte_line_is_allowed()
{
    CliRegistry registry;
    std::size_t received_size = 0U;

    assert(
        registry.register_command(
            "x",
            "line test",
            [&](const CliArguments& arguments) {
                assert(arguments.size() == 1U);
                received_size = arguments[0].size();
                return std::string{"ok\n"};
            })
        == CliRegisterStatus::SUCCESS);

    const std::string line =
        "x " + std::string(253U, 'a');

    assert(line.size() == CLI_MAX_LINE_SIZE);

    const CliDispatchResult result = registry.dispatch(line);

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(received_size == 253U);
}

/*
 * 超过 255 字节时，在解析和调用 Handler 前拒绝。
 */
void test_line_over_255_bytes_is_rejected()
{
    CliRegistry registry;
    std::size_t call_count = 0U;

    assert(
        registry.register_command(
            "x",
            "line test",
            [&](const CliArguments&) {
                ++call_count;
                return std::string{"unexpected\n"};
            })
        == CliRegisterStatus::SUCCESS);

    const std::string line =
        "x " + std::string(254U, 'a');

    assert(line.size() == CLI_MAX_LINE_SIZE + 1U);

    const CliDispatchResult result = registry.dispatch(line);

    assert(result._status == CliDispatchStatus::LINE_TOO_LONG);
    assert(result._output == "error: line too long\n");
    assert(call_count == 0U);
}

/*
 * 标准异常不能穿透到调用 Registry 的 Engine AIO 路径。
 */
void test_standard_exception_is_isolated()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "throws",
            "throw std exception",
            [](const CliArguments&) -> std::string {
                throw std::runtime_error{"handler failed"};
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("throws");

    assert(result._status == CliDispatchStatus::HANDLER_ERROR);
    assert(result._output == "error: command failed\n");
}

/*
 * 非 std::exception 异常也由最后一层 catch(...) 隔离。
 */
void test_unknown_exception_is_isolated()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "throws",
            "throw unknown exception",
            [](const CliArguments&) -> std::string {
                throw 42;
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("throws");

    assert(result._status == CliDispatchStatus::HANDLER_ERROR);
    assert(result._output == "error: command failed\n");
}

/*
 * outer Handler 再次 dispatch leaf，验证调用 Handler 时未持有锁。
 */
void test_handler_can_reenter_registry()
{
    CliRegistry registry;

    assert(
        registry.register_command(
            "leaf",
            "leaf command",
            make_handler("leaf result\n"))
        == CliRegisterStatus::SUCCESS);

    assert(
        registry.register_command(
            "outer",
            "reentrant command",
            [&registry](const CliArguments&) {
                const CliDispatchResult nested =
                    registry.dispatch("leaf");

                assert(nested._status == CliDispatchStatus::SUCCESS);
                return nested._output;
            })
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("outer");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "leaf result\n");
}

/*
 * commands() 按名字典序返回副本，修改快照不影响 Registry。
 */
void test_commands_returns_sorted_snapshot()
{
    CliRegistry registry;

    assert(
        registry.register_command("zeta", "last", make_handler())
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("alpha", "first", make_handler())
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("middle", "middle", make_handler())
        == CliRegisterStatus::SUCCESS);

    std::vector<CliCommandInfo> snapshot = registry.commands();

    assert(snapshot.size() == 3U);
    assert(snapshot[0]._name == "alpha");
    assert(snapshot[1]._name == "middle");
    assert(snapshot[2]._name == "zeta");

    snapshot[0]._name = "modified";
    snapshot[0]._help = "modified";

    const std::vector<CliCommandInfo> second = registry.commands();

    assert(second[0]._name == "alpha");
    assert(second[0]._help == "first");
}

/*
 * 注册、分派和快照可以由多个线程同时执行。
 */
void test_concurrent_registration_dispatch_and_snapshot()
{
    CliRegistry registry;
    std::atomic<std::size_t> call_count{0U};

    assert(
        registry.register_command(
            "ping",
            "ping command",
            [&call_count](const CliArguments&) {
                call_count.fetch_add(1U, std::memory_order_relaxed);
                return std::string{"pong\n"};
            })
        == CliRegisterStatus::SUCCESS);

    std::thread writer{
        [&registry] {
            for (std::size_t index = 0U;
                 index < kWriterCommands;
                 ++index) {
                assert(
                    registry.register_command(
                        "command-" + std::to_string(index),
                        "concurrent command",
                        make_handler())
                    == CliRegisterStatus::SUCCESS);
            }
        }};

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);

    for (std::size_t index = 0U;
         index < kReaderThreads;
         ++index) {
        readers.emplace_back(
            [&registry] {
                for (std::size_t call = 0U;
                     call < kCallsPerReader;
                     ++call) {
                    const CliDispatchResult result =
                        registry.dispatch("ping");

                    assert(result._status == CliDispatchStatus::SUCCESS);
                    assert(!registry.commands().empty());
                }
            });
    }

    writer.join();

    for (std::thread& reader : readers) {
        reader.join();
    }

    assert(
        call_count.load(std::memory_order_relaxed)
        == kReaderThreads * kCallsPerReader);

    assert(registry.commands().size() == kWriterCommands + 1U);
}

} // namespace

int main()
{
    test_register_and_dispatch();
    test_invalid_command_names_are_rejected();
    test_empty_handler_is_rejected();
    test_duplicate_command_is_rejected();
    test_command_name_is_case_sensitive();
    test_registry_owns_registered_values();
    test_empty_input_is_rejected();
    test_unknown_command();
    test_whitespace_tokenization();
    test_eight_tokens_are_allowed();
    test_nine_tokens_are_rejected();
    test_255_byte_line_is_allowed();
    test_line_over_255_bytes_is_rejected();
    test_standard_exception_is_isolated();
    test_unknown_exception_is_isolated();
    test_handler_can_reenter_registry();
    test_commands_returns_sorted_snapshot();
    test_concurrent_registration_dispatch_and_snapshot();

    std::cout << "CLI_REGISTRY_TEST=PASS\n";
    return 0;
}
