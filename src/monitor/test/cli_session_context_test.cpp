#include "cli_types.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

using namespace TLSSMON;

/* 新 Session 默认表示 all，不选择具体模块。 */
void test_default_context_has_no_selection()
{
    const CliSessionContext context;

    assert(!context.has_selected_module());
    assert(!context._selected_mid.has_value());
    assert(context._selected_module_name.empty());
}

/* Context 同时保存过滤使用的 mid 和提示符使用的名称。 */
void test_context_stores_selection()
{
    CliSessionContext context;
    context._selected_mid = 256U;
    context._selected_module_name = "module-a";

    assert(context.has_selected_module());
    assert(*context._selected_mid == 256U);
    assert(context._selected_module_name == "module-a");
}

/* clear_module() 是 use all 所需的幂等复位操作。 */
void test_clear_module_is_idempotent()
{
    CliSessionContext context;
    context._selected_mid = 256U;
    context._selected_module_name = "module-a";

    context.clear_module();
    context.clear_module();

    assert(!context.has_selected_module());
    assert(!context._selected_mid.has_value());
    assert(context._selected_module_name.empty());
}

/* mid 是选择状态的权威字段，单独设置名称不算完成选择。 */
void test_mid_is_authoritative()
{
    CliSessionContext context;
    context._selected_module_name = "module-a";
    assert(!context.has_selected_module());

    context._selected_mid = 256U;
    assert(context.has_selected_module());
}

/* 不同连接持有的 Context 不共享状态。 */
void test_contexts_are_independent()
{
    CliSessionContext first;
    CliSessionContext second;

    first._selected_mid = 256U;
    first._selected_module_name = "module-a";
    second._selected_mid = 512U;
    second._selected_module_name = "module-b";

    first.clear_module();

    assert(!first.has_selected_module());
    assert(second.has_selected_module());
    assert(*second._selected_mid == 512U);
    assert(second._selected_module_name == "module-b");
}

/* Session Handler 能通过引用修改调用者拥有的 Context。 */
void test_session_handler_updates_context()
{
    CliSessionContext context;
    const CliSessionHandler handler =
        [](CliSessionContext& session, const CliArguments& arguments) {
            assert(arguments.size() == 1U);
            session._selected_mid = 256U;
            session._selected_module_name = arguments[0];
            return std::string{"selected\n"};
        };

    assert(handler(context, {"module-a"}) == "selected\n");
    assert(*context._selected_mid == 256U);
    assert(context._selected_module_name == "module-a");
}

/* 固定新旧 Handler 的签名，并保持旧接口不变。 */
void test_handler_type_contracts()
{
    using ExpectedSessionHandler =
        std::function<std::string(CliSessionContext&, const CliArguments&)>;
    using ExpectedLegacyHandler =
        std::function<std::string(const CliArguments&)>;

    static_assert(std::is_same_v<CliSessionHandler, ExpectedSessionHandler>);
    static_assert(std::is_same_v<CliHandler, ExpectedLegacyHandler>);
    static_assert(std::is_copy_constructible_v<CliSessionContext>);
    static_assert(std::is_move_constructible_v<CliSessionContext>);
    static_assert(std::is_copy_constructible_v<CliSessionHandler>);
    static_assert(std::is_move_constructible_v<CliSessionHandler>);
}

/* Context 副本拥有自己的字符串和 optional 状态。 */
void test_context_copy_is_independent()
{
    CliSessionContext original;
    original._selected_mid = 256U;
    original._selected_module_name = "module-a";

    CliSessionContext copy = original;
    copy._selected_mid = 512U;
    copy._selected_module_name = "module-b";

    assert(*original._selected_mid == 256U);
    assert(original._selected_module_name == "module-a");
    assert(*copy._selected_mid == 512U);
    assert(copy._selected_module_name == "module-b");
}

} // namespace

int main()
{
    test_default_context_has_no_selection();
    test_context_stores_selection();
    test_clear_module_is_idempotent();
    test_mid_is_authoritative();
    test_contexts_are_independent();
    test_session_handler_updates_context();
    test_handler_type_contracts();
    test_context_copy_is_independent();

    std::cout << "CLI_SESSION_CONTEXT=PASS\n";
    return 0;
}
