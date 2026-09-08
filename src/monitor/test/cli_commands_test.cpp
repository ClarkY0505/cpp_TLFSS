#include "cli_commands.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace TLSSMON;

CliHandler make_handler(std::string output = "ok\n")
{
    return [output = std::move(output)](
               const CliArguments&) {
        return output;
    };
}

/*
 * help 注册成功，并以正确的说明出现在命令快照中。
 */
void test_register_help_command()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    const std::vector<CliCommandInfo> commands =
        registry.commands();

    assert(commands.size() == 1U);
    assert(commands[0]._name == "help");
    assert(commands[0]._help == "list available commands");
}

/*
 * help 必须能够列出自身。
 */
void test_help_lists_itself()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "help    list available commands\n");
}

/*
 * help 每次执行时读取新快照，后注册的命令也会显示。
 */
void test_help_lists_commands_registered_later()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    assert(
        registry.register_command(
            "db_dump",
            "dump monitor records",
            make_handler())
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output
        == "db_dump    dump monitor records\n"
           "help       list available commands\n");
}

/*
 * help 输出顺序稳定为命令名字典序。
 */
void test_help_output_is_sorted()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("zeta", "last command", make_handler())
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("alpha", "first command", make_handler())
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("middle", "middle command", make_handler())
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output
        == "alpha     first command\n"
           "help      list available commands\n"
           "middle    middle command\n"
           "zeta      last command\n");
}

/*
 * Registry 按值持有说明文本，调用者修改原字符串不影响 help。
 */
void test_help_uses_owned_help_text()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    std::string name{"status"};
    std::string help{"show engine status"};

    assert(
        registry.register_command(
            name,
            help,
            make_handler())
        == CliRegisterStatus::SUCCESS);

    name.clear();
    help.clear();

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output.find("status    show engine status\n")
        != std::string::npos);
}

/*
 * 空说明合法，输出行不应包含无意义的尾随空格。
 */
void test_help_handles_empty_description()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command("empty", "", make_handler())
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(
        result._output
        == "empty\n"
           "help     list available commands\n");
}

/*
 * 与参考 M7 一致，help 忽略额外参数。
 */
void test_help_ignores_arguments()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    const CliDispatchResult result =
        registry.dispatch("help ignored arguments");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "help    list available commands\n");
}

/*
 * 重复注册 help 被拒绝，旧 Handler 不被替换。
 */
void test_duplicate_help_registration_is_rejected()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);
    assert(
        register_help_command(registry)
        == CliRegisterStatus::DUPLICATE_COMMAND);

    assert(registry.commands().size() == 1U);

    const CliDispatchResult result =
        registry.dispatch("help");

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "help    list available commands\n");
}

/*
 * help Handler 会重新进入 Registry::commands()。
 * 如果 dispatch() 持锁调用 Handler，本案例会死锁。
 */
void test_help_reenters_registry_without_deadlock()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);

    for (std::size_t index = 0U; index < 10U; ++index) {
        const CliDispatchResult result =
            registry.dispatch("help");

        assert(result._status == CliDispatchStatus::SUCCESS);
        assert(!result._output.empty());
    }
}

/*
 * 并发注册期间执行 help，只能读取独立快照，不能持有失效引用。
 */
void test_help_is_safe_during_concurrent_registration()
{
    CliRegistry registry;

    assert(
        register_help_command(registry)
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command(
            "stable",
            "stable command",
            make_handler())
        == CliRegisterStatus::SUCCESS);

    constexpr std::size_t command_count = 100U;
    constexpr std::size_t help_calls = 300U;

    std::atomic<bool> start{false};

    std::thread writer{
        [&registry, &start] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0U;
                 index < command_count;
                 ++index) {
                assert(
                    registry.register_command(
                        "dynamic-" + std::to_string(index),
                        "dynamic command",
                        make_handler())
                    == CliRegisterStatus::SUCCESS);
            }
        }};

    std::thread reader{
        [&registry, &start] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0U;
                 index < help_calls;
                 ++index) {
                const CliDispatchResult result =
                    registry.dispatch("help");

                assert(result._status == CliDispatchStatus::SUCCESS);
                assert(result._output.find("help") != std::string::npos);
                assert(result._output.find("stable") != std::string::npos);
            }
        }};

    start.store(true, std::memory_order_release);

    writer.join();
    reader.join();

    const CliDispatchResult final_result =
        registry.dispatch("help");

    assert(final_result._status == CliDispatchStatus::SUCCESS);

    const std::size_t line_count =
        static_cast<std::size_t>(
            std::count(
                final_result._output.begin(),
                final_result._output.end(),
                '\n'));

    assert(line_count == command_count + 2U);
}

} // namespace

int main()
{
    test_register_help_command();
    test_help_lists_itself();
    test_help_lists_commands_registered_later();
    test_help_output_is_sorted();
    test_help_uses_owned_help_text();
    test_help_handles_empty_description();
    test_help_ignores_arguments();
    test_duplicate_help_registration_is_rejected();
    test_help_reenters_registry_without_deadlock();
    test_help_is_safe_during_concurrent_registration();

    std::cout << "CLI_COMMANDS_HELP_TEST=PASS\n";
    return 0;
}
