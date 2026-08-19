#include "engine.h"

#include <type_traits>
#include <utility>

using namespace TLSSMON;
/*
 * 锁定 update_data() 的完整成员函数类型：
 *
 * 返回值：MonData::UpdateResult
 * 参数 1：MonData::MonitorData，按值传递
 * 参数 2：bool
 */
using ExpectedUpdateData =
    MonData::UpdateResult (Engine::*)(MonData::MonitorData, bool);

static_assert(
    std::is_same_v<decltype(&Engine::update_data), ExpectedUpdateData>,
    "Engine::update_data signature changed");

/*
 * 成员函数指针类型不包含默认参数信息。
 *
 * 因此需要额外验证：
 *
 * engine.update_data(data)
 *
 * 这种只传 MonitorData 的现有调用方式仍然可以编译，
 * 也就是 bool force 继续保持默认值 false。
 */
using DefaultForceCallResult = decltype(std::declval<Engine &>().update_data(
    std::declval<MonData::MonitorData>()));

static_assert(std::is_same_v<DefaultForceCallResult, MonData::UpdateResult>,
              "Engine::update_data(data) must remain valid");

/*
 * 同时验证显式传入 force 的调用方式。
 */
using ExplicitForceCallResult = decltype(std::declval<Engine &>().update_data(
    std::declval<MonData::MonitorData>(), true));

static_assert(std::is_same_v<ExplicitForceCallResult, MonData::UpdateResult>,
              "Engine::update_data(data, force) must remain valid");

int main() { return 0; }
