#include "monitor_module.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

using namespace TLSSMON;

/* 固定模块注册状态的数量和底层整数类型。 */
void test_register_status_contract()
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<ModuleRegisterStatus>,
            std::uint8_t>);

    constexpr std::array<ModuleRegisterStatus, 4U> statuses{
        ModuleRegisterStatus::SUCCESS,
        ModuleRegisterStatus::INVALID_NAME,
        ModuleRegisterStatus::DUPLICATE_ID,
        ModuleRegisterStatus::DUPLICATE_NAME
    };

    assert(statuses.size() == 4U);
}

/* 名称上限固定为 63 字节。 */
void test_name_length_limit()
{
    static_assert(MONITOR_MODULE_NAME_MAX == 63U);

    assert(is_valid_module_name(std::string(63U, 'a')));
    assert(!is_valid_module_name(std::string(64U, 'a')));
}

/* 常用安全 ASCII 名称必须被接受。 */
void test_valid_names()
{
    const std::array<std::string, 8U> names{
        "module-a",
        "metadata_v2",
        "chunk.store",
        "Module-A",
        "A",
        "0",
        "All",
        "a-b_c.d9"
    };

    for (const std::string& name : names) {
        assert(is_valid_module_name(name));
    }
}

/* 空名称、保留名称和可能破坏提示符/分词的字符必须被拒绝。 */
void test_invalid_names()
{
    const std::array<std::string, 12U> names{
        "",
        "all",
        "module a",
        "module\ta",
        "module\na",
        "module\ra",
        "[module]",
        "module/a",
        "module:a",
        "module>a",
        "module<a",
        "module@a"
    };

    for (const std::string& name : names) {
        assert(!is_valid_module_name(name));
    }
}

/* 内嵌 NUL 和非 ASCII 高位字节不能进入模块名。 */
void test_binary_and_non_ascii_names_are_rejected()
{
    const std::string embedded_nul{"module\0a", 8U};
    const std::string high_byte{
        static_cast<char>(0xE4),
        static_cast<char>(0xB8),
        static_cast<char>(0xAD)
    };

    assert(!is_valid_module_name(embedded_nul));
    assert(!is_valid_module_name(high_byte));
}

/* 单字符校验函数可以在编译期使用。 */
void test_character_validator()
{
    static_assert(is_valid_module_name_character('a'));
    static_assert(is_valid_module_name_character('Z'));
    static_assert(is_valid_module_name_character('7'));
    static_assert(is_valid_module_name_character('-'));
    static_assert(is_valid_module_name_character('_'));
    static_assert(is_valid_module_name_character('.'));
    static_assert(!is_valid_module_name_character(' '));
    static_assert(!is_valid_module_name_character('['));
    static_assert(!is_valid_module_name_character(0xFFU));
}

/* mid 为 0 是合法的公共数据，不增加隐藏限制。 */
void test_zero_mid_is_representable()
{
    const MonitorModuleInfo module{
        0U,
        "system",
        "system module"
    };

    assert(module._mid == 0U);
    assert(module._name == "system");
    assert(module._description == "system module");
}

/* 完整值语义包含 mid、name 和 description。 */
void test_module_info_value_semantics()
{
    const MonitorModuleInfo first{1U, "module-a", "first"};
    const MonitorModuleInfo same{1U, "module-a", "first"};
    const MonitorModuleInfo different_id{2U, "module-a", "first"};
    const MonitorModuleInfo different_name{1U, "module-b", "first"};
    const MonitorModuleInfo different_description{1U, "module-a", "second"};

    assert(first == same);
    assert(!(first != same));
    assert(first != different_id);
    assert(first != different_name);
    assert(first != different_description);

    static_assert(std::is_copy_constructible_v<MonitorModuleInfo>);
    static_assert(std::is_move_constructible_v<MonitorModuleInfo>);
}

} // namespace

int main()
{
    test_register_status_contract();
    test_name_length_limit();
    test_valid_names();
    test_invalid_names();
    test_binary_and_non_ascii_names_are_rejected();
    test_character_validator();
    test_zero_mid_is_representable();
    test_module_info_value_semantics();

    std::cout << "MONITOR_MODULE_TYPES=PASS\n";
    return 0;
}
