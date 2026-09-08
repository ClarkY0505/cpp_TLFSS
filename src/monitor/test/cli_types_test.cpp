#include "cli_types.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace TLSSMON;

/*
 * 验证注册状态集合，并固定枚举底层类型。
 */
void test_register_status_type()
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<CliRegisterStatus>,
            std::uint8_t>);

    constexpr std::array<CliRegisterStatus, 4U> statuses{
        CliRegisterStatus::SUCCESS,
        CliRegisterStatus::INVALID_NAME,
        CliRegisterStatus::INVALID_HANDLER,
        CliRegisterStatus::DUPLICATE_COMMAND
    };

    assert(statuses.size() == 4U);
}

/*
 * 验证分派状态集合，并固定枚举底层类型。
 */
void test_dispatch_status_type()
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<CliDispatchStatus>,
            std::uint8_t>);

    constexpr std::array<CliDispatchStatus, 6U> statuses{
        CliDispatchStatus::SUCCESS,
        CliDispatchStatus::EMPTY_INPUT,
        CliDispatchStatus::UNKNOWN_COMMAND,
        CliDispatchStatus::TOO_MANY_TOKENS,
        CliDispatchStatus::LINE_TOO_LONG,
        CliDispatchStatus::HANDLER_ERROR
    };

    assert(statuses.size() == 6U);
}

/*
 * 固定阶段 0 约定的行长、token 和并发客户端上限。
 */
void test_cli_limits()
{
    static_assert(CLI_MAX_LINE_SIZE == 255U);
    static_assert(CLI_MAX_TOKENS == 8U);
    static_assert(CLI_MAX_CLIENTS == 8U);
}

/*
 * Handler 可以捕获外部状态，并且参数列表不需要附带命令名。
 */
void test_handler_can_capture_state()
{
    std::size_t call_count = 0U;

    CliHandler handler =
        [&call_count](const CliArguments& arguments) {
            ++call_count;
            assert(arguments.size() == 2U);
            assert(arguments[0] == "first");
            assert(arguments[1] == "second");
            return arguments[0] + ':' + arguments[1];
        };

    const std::string output =
        handler(CliArguments{"first", "second"});

    assert(call_count == 1U);
    assert(output == "first:second");
}

/*
 * std::string 按长度保存输出，内嵌 NUL 不能被截断。
 */
void test_handler_preserves_embedded_nul()
{
    const CliHandler handler =
        [](const CliArguments&) {
            return std::string{"A\0B", 3U};
        };

    const std::string output = handler({});

    assert(output.size() == 3U);
    assert(output[0] == 'A');
    assert(output[1] == '\0');
    assert(output[2] == 'B');
}

/*
 * 空 std::function 用来表达非法的空 Handler。
 */
void test_empty_handler_is_representable()
{
    const CliHandler handler{};
    assert(!handler);
}

/*
 * 分派结果拥有输出副本，不依赖调用者字符串的生命周期。
 */
void test_dispatch_result_owns_output()
{
    std::string source{"X\0Y", 3U};

    CliDispatchResult result{
        CliDispatchStatus::SUCCESS,
        source
    };

    source[0] = 'Z';

    assert(result._status == CliDispatchStatus::SUCCESS);
    assert(result._output.size() == 3U);
    assert(result._output[0] == 'X');
    assert(result._output[1] == '\0');
    assert(result._output[2] == 'Y');
}

/*
 * 命令快照拥有命令名和帮助文本的副本。
 */
void test_command_info_owns_strings()
{
    std::string name{"db_dump"};
    std::string help{"dump monitor records"};

    CliCommandInfo info{name, help};

    name.clear();
    help.clear();

    assert(info._name == "db_dump");
    assert(info._help == "dump monitor records");
}

/*
 * 公共对象必须支持按值复制和移动，以便安全返回快照。
 */
void test_public_types_have_value_semantics()
{
    static_assert(std::is_copy_constructible_v<CliArguments>);
    static_assert(std::is_move_constructible_v<CliArguments>);
    static_assert(std::is_copy_constructible_v<CliHandler>);
    static_assert(std::is_move_constructible_v<CliHandler>);
    static_assert(std::is_copy_constructible_v<CliDispatchResult>);
    static_assert(std::is_move_constructible_v<CliDispatchResult>);
    static_assert(std::is_copy_constructible_v<CliCommandInfo>);
    static_assert(std::is_move_constructible_v<CliCommandInfo>);
}

/*
 * 固定 Handler 的完整接口签名。
 */
void test_handler_signature()
{
    using ExpectedHandler =
        std::function<std::string(const CliArguments&)>;

    static_assert(std::is_same_v<CliHandler, ExpectedHandler>);
}

} // namespace

int main()
{
    test_register_status_type();
    test_dispatch_status_type();
    test_cli_limits();
    test_handler_can_capture_state();
    test_handler_preserves_embedded_nul();
    test_empty_handler_is_representable();
    test_dispatch_result_owns_output();
    test_command_info_owns_strings();
    test_public_types_have_value_semantics();
    test_handler_signature();

    std::cout << "CLI_TYPES_TEST=PASS\n";
    return 0;
}
