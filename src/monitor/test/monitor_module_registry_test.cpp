#include "monitor_module_registry.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace TLSSMON;

/* Registry 不能复制或移动，避免互斥锁和内部索引被意外分离。 */
void test_registry_ownership_contract()
{
    static_assert(!std::is_copy_constructible_v<MonitorModuleRegistry>);
    static_assert(!std::is_copy_assignable_v<MonitorModuleRegistry>);
    static_assert(!std::is_move_constructible_v<MonitorModuleRegistry>);
    static_assert(!std::is_move_assignable_v<MonitorModuleRegistry>);
}

/* 注册后可以分别通过 mid 和名称取得完整副本。 */
void test_register_and_find()
{
    MonitorModuleRegistry registry;
    const MonitorModuleInfo module{256U, "module-a", "metadata service"};

    assert(registry.register_module(module) == ModuleRegisterStatus::SUCCESS);
    assert(registry.find_by_id(256U) == module);
    assert(registry.find_by_name("module-a") == module);
    assert(!registry.find_by_id(512U).has_value());
    assert(!registry.find_by_name("missing").has_value());
}

/* 非法名称不能在任意索引中留下数据。 */
void test_invalid_name_does_not_modify_registry()
{
    MonitorModuleRegistry registry;

    assert(
        registry.register_module({1U, "all", "reserved"})
        == ModuleRegisterStatus::INVALID_NAME);
    assert(registry.modules().empty());
    assert(!registry.find_by_id(1U).has_value());
    assert(!registry.find_by_name("all").has_value());
}

/* 重复 mid 不覆盖旧记录，也不能留下新名称索引。 */
void test_duplicate_id_is_atomic()
{
    MonitorModuleRegistry registry;
    const MonitorModuleInfo original{10U, "module-a", "original"};

    assert(registry.register_module(original) == ModuleRegisterStatus::SUCCESS);
    assert(
        registry.register_module({10U, "module-b", "replacement"})
        == ModuleRegisterStatus::DUPLICATE_ID);

    assert(registry.find_by_id(10U) == original);
    assert(!registry.find_by_name("module-b").has_value());
    assert(registry.modules().size() == 1U);
}

/* 重复名称不覆盖旧记录，也不能留下新 mid 索引。 */
void test_duplicate_name_is_atomic()
{
    MonitorModuleRegistry registry;
    const MonitorModuleInfo original{10U, "module-a", "original"};

    assert(registry.register_module(original) == ModuleRegisterStatus::SUCCESS);
    assert(
        registry.register_module({20U, "module-a", "replacement"})
        == ModuleRegisterStatus::DUPLICATE_NAME);

    assert(registry.find_by_name("module-a") == original);
    assert(!registry.find_by_id(20U).has_value());
    assert(registry.modules().size() == 1U);
}

/* 名称区分大小写，且 mid 0 可以注册。 */
void test_case_sensitive_names_and_zero_mid()
{
    MonitorModuleRegistry registry;

    assert(
        registry.register_module({0U, "module-a", "lower"})
        == ModuleRegisterStatus::SUCCESS);
    assert(
        registry.register_module({1U, "Module-A", "upper"})
        == ModuleRegisterStatus::SUCCESS);

    assert(registry.find_by_name("module-a")->_mid == 0U);
    assert(registry.find_by_name("Module-A")->_mid == 1U);
    assert(!registry.find_by_name("MODULE-A").has_value());
}

/* modules() 必须按名称而不是 mid 排序。 */
void test_snapshot_is_sorted_by_name()
{
    MonitorModuleRegistry registry;

    assert(registry.register_module({3U, "zeta", "z"}) == ModuleRegisterStatus::SUCCESS);
    assert(registry.register_module({1U, "alpha", "a"}) == ModuleRegisterStatus::SUCCESS);
    assert(registry.register_module({2U, "metadata", "m"}) == ModuleRegisterStatus::SUCCESS);

    const auto snapshot = registry.modules();
    assert(snapshot.size() == 3U);
    assert(snapshot[0]._name == "alpha");
    assert(snapshot[1]._name == "metadata");
    assert(snapshot[2]._name == "zeta");
}

/* 查询结果是副本，调用者修改它不能影响 Registry。 */
void test_queries_return_independent_copies()
{
    MonitorModuleRegistry registry;
    const MonitorModuleInfo original{10U, "module-a", "original"};

    assert(registry.register_module(original) == ModuleRegisterStatus::SUCCESS);

    auto by_id = registry.find_by_id(10U);
    auto by_name = registry.find_by_name("module-a");
    auto snapshot = registry.modules();

    by_id->_description = "changed by id";
    by_name->_description = "changed by name";
    snapshot[0]._description = "changed snapshot";

    assert(registry.find_by_id(10U) == original);
    assert(registry.find_by_name("module-a") == original);
}

/* 多线程注册不同模块并同时查询，不得观察到半注册状态。 */
void test_concurrent_registration_and_query()
{
    constexpr std::size_t writer_count = 6U;
    constexpr std::size_t reader_count = 3U;
    constexpr std::size_t modules_per_writer = 80U;

    MonitorModuleRegistry registry;
    std::atomic<bool> start{false};
    std::atomic<std::size_t> finished_writers{0U};
    std::vector<std::thread> threads;

    for (std::size_t reader = 0U; reader < reader_count; ++reader) {
        threads.emplace_back([&registry, &start, &finished_writers] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (finished_writers.load(std::memory_order_acquire)
                   != writer_count) {
                const auto snapshot = registry.modules();
                assert(std::is_sorted(
                    snapshot.begin(),
                    snapshot.end(),
                    [](const MonitorModuleInfo& lhs,
                       const MonitorModuleInfo& rhs) {
                        return lhs._name < rhs._name;
                    }));

                for (const auto& module : snapshot) {
                    assert(registry.find_by_id(module._mid).has_value());
                    assert(registry.find_by_name(module._name).has_value());
                }
            }
        });
    }

    for (std::size_t writer = 0U; writer < writer_count; ++writer) {
        threads.emplace_back([&registry, &start, &finished_writers, writer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0U; index < modules_per_writer; ++index) {
                const auto mid = static_cast<std::uint32_t>(
                    writer * modules_per_writer + index);
                const std::string name = "module-" + std::to_string(mid);

                assert(
                    registry.register_module({mid, name, "concurrent"})
                    == ModuleRegisterStatus::SUCCESS);
            }

            finished_writers.fetch_add(1U, std::memory_order_release);
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    assert(
        registry.modules().size()
        == writer_count * modules_per_writer);
}

/* 相同 mid 的并发注册只能有一个成功。 */
void test_concurrent_duplicate_id_has_one_winner()
{
    constexpr std::size_t thread_count = 16U;

    MonitorModuleRegistry registry;
    std::atomic<bool> start{false};
    std::atomic<std::size_t> successes{0U};
    std::vector<std::thread> threads;

    for (std::size_t index = 0U; index < thread_count; ++index) {
        threads.emplace_back([&registry, &start, &successes, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const auto status = registry.register_module(
                {42U, "candidate-" + std::to_string(index), "candidate"});

            if (status == ModuleRegisterStatus::SUCCESS) {
                successes.fetch_add(1U, std::memory_order_relaxed);
            } else {
                assert(status == ModuleRegisterStatus::DUPLICATE_ID);
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    assert(successes.load(std::memory_order_relaxed) == 1U);
    assert(registry.modules().size() == 1U);
}

} // namespace

int main()
{
    test_registry_ownership_contract();
    test_register_and_find();
    test_invalid_name_does_not_modify_registry();
    test_duplicate_id_is_atomic();
    test_duplicate_name_is_atomic();
    test_case_sensitive_names_and_zero_mid();
    test_snapshot_is_sorted_by_name();
    test_queries_return_independent_copies();
    test_concurrent_registration_and_query();
    test_concurrent_duplicate_id_has_one_winner();

    std::cout << "MODULE_REGISTRY=PASS\n";
    return 0;
}
