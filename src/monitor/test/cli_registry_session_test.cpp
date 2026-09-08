#include "cli_registry.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace TLSSMON;

/* Session Handler 可以修改本次 dispatch() 收到的 Context。 */
void test_session_handler_updates_context()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "use",
            "select module",
            [](CliSessionContext& context, const CliArguments& arguments) {
                assert(arguments.size() == 1U);
                context._selected_mid = 256U;
                context._selected_module_name = arguments[0];
                return std::string{"using module: module-a\n"};
            })
        == CliRegisterStatus::SUCCESS);

    CliSessionContext context;
    const auto result = registry.dispatch("use module-a", context);

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output == "using module: module-a\n");
    assert(*context._selected_mid == 256U);
    assert(context._selected_module_name == "module-a");
}

/* 同一 Context 的选择状态可以跨多次分派保留。 */
void test_context_persists_between_dispatches()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "select",
            "select module-a",
            [](CliSessionContext& context, const CliArguments&) {
                context._selected_mid = 256U;
                context._selected_module_name = "module-a";
                return std::string{"selected\n"};
            })
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command(
            "current",
            "show current module",
            [](CliSessionContext& context, const CliArguments&) {
                return context.has_selected_module()
                    ? "current module: " + context._selected_module_name + '\n'
                    : std::string{"current module: all\n"};
            })
        == CliRegisterStatus::SUCCESS);

    CliSessionContext context;
    assert(registry.dispatch("current", context)._output == "current module: all\n");
    assert(registry.dispatch("select", context)._output == "selected\n");
    assert(registry.dispatch("current", context)._output == "current module: module-a\n");
}

/* 使用同一个 Registry 的两个 Context 仍然相互独立。 */
void test_two_contexts_are_independent()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "use",
            "select module",
            [](CliSessionContext& context, const CliArguments& arguments) {
                assert(arguments.size() == 2U);
                context._selected_mid = static_cast<std::uint32_t>(
                    std::stoul(arguments[0]));
                context._selected_module_name = arguments[1];
                return std::string{"ok\n"};
            })
        == CliRegisterStatus::SUCCESS);

    CliSessionContext first;
    CliSessionContext second;
    assert(registry.dispatch("use 256 module-a", first)._status == CliDispatchStatus::SUCCESS);
    assert(registry.dispatch("use 512 module-b", second)._status == CliDispatchStatus::SUCCESS);
    assert(*first._selected_mid == 256U);
    assert(first._selected_module_name == "module-a");
    assert(*second._selected_mid == 512U);
    assert(second._selected_module_name == "module-b");
}

/* 旧 Handler 可由新 dispatch() 调用，且不会改变 Context。 */
void test_legacy_handler_works_with_session_dispatch()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "ping",
            "return pong",
            CliHandler{[](const CliArguments&) {
                return std::string{"pong\n"};
            }})
        == CliRegisterStatus::SUCCESS);

    CliSessionContext context;
    context._selected_mid = 256U;
    context._selected_module_name = "module-a";

    assert(registry.dispatch("ping", context)._output == "pong\n");
    assert(*context._selected_mid == 256U);
    assert(context._selected_module_name == "module-a");
}

/* 旧 dispatch(line) 接口继续工作，并为新 Handler 使用临时 Context。 */
void test_legacy_dispatch_uses_temporary_context()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "current",
            "show current module",
            [](CliSessionContext& context, const CliArguments&) {
                assert(!context.has_selected_module());
                return std::string{"current module: all\n"};
            })
        == CliRegisterStatus::SUCCESS);

    assert(registry.dispatch("current")._output == "current module: all\n");
}

/* 空 Session Handler 和非法命令名必须被拒绝。 */
void test_session_registration_validation()
{
    CliRegistry registry;

    assert(
        registry.register_command("empty", "empty", CliSessionHandler{})
        == CliRegisterStatus::INVALID_HANDLER);
    assert(
        registry.register_command(
            "two words",
            "invalid",
            CliSessionHandler{[](CliSessionContext&, const CliArguments&) {
                return std::string{"ok\n"};
            }})
        == CliRegisterStatus::INVALID_NAME);
    assert(registry.commands().empty());
}

/* 两种 Handler 使用同一个命令命名空间，重复注册不能覆盖旧命令。 */
void test_handler_kinds_share_command_namespace()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "status",
            "legacy",
            CliHandler{[](const CliArguments&) {
                return std::string{"legacy\n"};
            }})
        == CliRegisterStatus::SUCCESS);
    assert(
        registry.register_command(
            "status",
            "session",
            CliSessionHandler{[](CliSessionContext&, const CliArguments&) {
                return std::string{"session\n"};
            }})
        == CliRegisterStatus::DUPLICATE_COMMAND);

    assert(registry.dispatch("status")._output == "legacy\n");
    assert(registry.commands().size() == 1U);
    assert(registry.commands()[0]._help == "legacy");
}

/* Session Handler 的异常转换为 HANDLER_ERROR。 */
void test_session_handler_exception_is_isolated()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "fail",
            "throw",
            [](CliSessionContext&, const CliArguments&) -> std::string {
                throw std::runtime_error{"failure"};
            })
        == CliRegisterStatus::SUCCESS);

    CliSessionContext context;
    const auto result = registry.dispatch("fail", context);
    assert(result._status == CliDispatchStatus::HANDLER_ERROR);
    assert(result._output == "error: command failed\n");
}

/* Handler 在 Registry 解锁后执行，因此可以重新进入 Registry。 */
void test_session_handler_can_reenter_registry()
{
    CliRegistry registry;
    assert(
        registry.register_command(
            "install",
            "install nested command",
            [&registry](CliSessionContext&, const CliArguments&) {
                const auto status = registry.register_command(
                    "nested",
                    "nested command",
                    CliHandler{[](const CliArguments&) {
                        return std::string{"nested\n"};
                    }});
                assert(status == CliRegisterStatus::SUCCESS);
                return std::string{"installed\n"};
            })
        == CliRegisterStatus::SUCCESS);

    CliSessionContext context;
    assert(registry.dispatch("install", context)._output == "installed\n");
    assert(registry.dispatch("nested", context)._output == "nested\n");
}

/* 分派错误发生在 Handler 前，不能改变已有 Session 选择。 */
void test_dispatch_error_does_not_change_context()
{
    CliRegistry registry;
    CliSessionContext context;
    context._selected_mid = 256U;
    context._selected_module_name = "module-a";

    const auto result = registry.dispatch("missing", context);
    assert(result._status == CliDispatchStatus::UNKNOWN_COMMAND);
    assert(*context._selected_mid == 256U);
    assert(context._selected_module_name == "module-a");
}

} // namespace

int main()
{
    test_session_handler_updates_context();
    test_context_persists_between_dispatches();
    test_two_contexts_are_independent();
    test_legacy_handler_works_with_session_dispatch();
    test_legacy_dispatch_uses_temporary_context();
    test_session_registration_validation();
    test_handler_kinds_share_command_namespace();
    test_session_handler_exception_is_isolated();
    test_session_handler_can_reenter_registry();
    test_dispatch_error_does_not_change_context();

    std::cout << "CLI_REGISTRY_SESSION=PASS\n";
    return 0;
}
