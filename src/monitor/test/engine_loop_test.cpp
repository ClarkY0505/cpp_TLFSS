#include "../engine.h"

#include <chrono>
#include <cassert>
#include <cstddef>
#include <dirent.h>
#include <string_view>
#include <future>
#include <thread>

using namespace TLSSMON;
static bool wait_for_phase(Engine& engine, EnginePhase target, std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == target) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == target;
}

static void test_run_requires_ready_engine()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.run() == ENGINESTATE::NOTREADY);
    assert(engine.get_phase() == EnginePhase::CREATED);
}

static void test_stop_before_run_is_noop()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::READY);

    engine.stop();

    assert(engine.get_phase() == EnginePhase::READY);
}

static void test_run_blocks_until_stop()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    std::future<ENGINESTATE> result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    // 进入 RUNNING 后，run() 必须仍阻塞在 select()。
    assert(result.wait_for(50ms)
           == std::future_status::timeout);

    engine.stop();

    // stop() 必须通过 WakeupPipe 及时打断 select()。
    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_second_run_is_rejected_while_running()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto first_result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    assert(engine.run() == ENGINESTATE::ALREADYRUNNING);
    assert(engine.get_phase() == EnginePhase::RUNNING);

    engine.stop();

    assert(first_result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(first_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_stop_is_idempotent()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    engine.stop();
    engine.stop();

    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    // STOPPED 状态再次调用也应当是空操作。
    engine.stop();
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_stopped_engine_cannot_restart()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    engine.stop();

    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();
    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    assert(engine.run() == ENGINESTATE::NOTREADY);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

int main(){
    test_run_requires_ready_engine();
    test_stop_before_run_is_noop();
    test_run_blocks_until_stop();
    test_second_run_is_rejected_while_running();
    test_stop_is_idempotent();
    test_stopped_engine_cannot_restart();

}
