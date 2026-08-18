#include "../engine.h"

#include <cassert>
#include <cstddef>
#include <dirent.h>
#include <string_view>

using namespace TLSSMON;

static std::size_t count_open_fds()
{
    DIR* directory = ::opendir("/proc/self/fd");
    assert(directory != nullptr);

    std::size_t count = 0;
    while (dirent* entry = ::readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name != "." && name != "..") {
            ++count;
        }
    }

    assert(::closedir(directory) == 0);
    return count;
}

static void test_engine_starts_in_created_phase()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.get_phase() == EnginePhase::CREATED);
}

static void test_invalid_config_restores_created_phase()
{
    const std::size_t fd_count_before = count_open_fds();
    Engine engine(MonConfig{"", 9000, 1});

    assert(engine.init() == ENGINESTATE::INVALIDCONFIG);
    assert(engine.get_phase() == EnginePhase::CREATED);
    assert(count_open_fds() == fd_count_before);

    // 验证失败后状态已恢复，而不是卡在 INITIALIZING。
    assert(engine.init() == ENGINESTATE::INVALIDCONFIG);
    assert(engine.get_phase() == EnginePhase::CREATED);
    assert(count_open_fds() == fd_count_before);
}

static void test_successful_init_enters_ready_phase()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.get_phase() == EnginePhase::CREATED);
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::READY);
}

static void test_initialized_engine_can_be_destroyed()
{
    const std::size_t fd_count_before = count_open_fds();

    {
        Engine engine(MonConfig{"monitor", 9000, 1});

        assert(engine.init() == ENGINESTATE::SUCCESSFUL);
        assert(engine.get_phase() == EnginePhase::READY);
        assert(count_open_fds() == fd_count_before + 2);
    }

    // 离开作用域时：
    // AioManager 注销 wakeup callback；
    // EnhancedCallback 析构；
    // WakeupPipe 关闭 fd；
    // 不应崩溃或留下 joinable thread。
    assert(count_open_fds() == fd_count_before);
}

static void test_second_init_is_rejected()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(
           engine.init()
           == ENGINESTATE::ALREADYINITIALIZED);

    assert(engine.get_phase() == EnginePhase::READY);
}

int main()
{
    test_engine_starts_in_created_phase();
    test_invalid_config_restores_created_phase();
    test_successful_init_enters_ready_phase();
    test_second_init_is_rejected();
    test_initialized_engine_can_be_destroyed();
}
