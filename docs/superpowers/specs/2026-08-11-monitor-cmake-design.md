# Monitor CMake 构建设计

## 背景

`src/monitor` 已从旧的 C 实现迁移到新的 C++ 实现，但原有
`src/monitor/CMakeLists.txt` 已删除。顶层构建仍通过
`add_subdirectory(src/monitor)` 引入该目录，并要求存在名为
`monitor_lib` 的链接目标，因此 CMake 在配置阶段失败。

新版 monitor 还包含 14 个各自带有 `main()` 的断言测试。这些测试不依赖
GoogleTest，但顶层配置当前会在找不到 GoogleTest 时把 `BUILD_TESTS` 改为
`OFF`，导致无法独立启用 monitor 测试。部分测试还引用了迁移前的
`../*.h` 路径，而头文件目前位于 `inc/monitor`。

## 目标

- 恢复顶层构建所需的 `monitor_lib` 静态库目标。
- 编译新版 monitor 的全部五个实现文件。
- 在 `BUILD_TESTS=ON` 时编译并注册全部 monitor 独立测试。
- monitor 测试不依赖 GoogleTest 是否安装。
- 保留现有基于 GoogleTest 的顶层测试；找不到 GoogleTest 时只跳过这部分测试。
- 让 `cmake --build --preset debug` 完成配置和编译，并能通过 CTest 运行 monitor 测试。

## 非目标

- 不把现有断言测试迁移到 GoogleTest。
- 不合并测试程序，因为每个测试源文件都有独立的 `main()`。
- 不修改 monitor 的运行逻辑或公共 API。
- 不重构其他模块的 CMake 配置。

## 构建目标设计

新增 `src/monitor/CMakeLists.txt`，显式创建 `monitor_lib` 静态库，并列出：

- `aio_manager.cpp`
- `callback_registry.cpp`
- `engine.cpp`
- `timer_manager.cpp`
- `wake_pipe.cpp`

`monitor_lib` 公开 `${CMAKE_SOURCE_DIR}/inc/monitor` 作为头文件目录，使库实现、
消费者和测试统一使用 `#include "engine.h"` 一类路径。该库公开链接
`Threads::Threads`，因为回调注册表和引擎 API 使用线程设施，链接该库的程序
需要继承线程链接参数。

新版实现没有直接使用 `common_lib`、`rpc_lib` 或 `proto_lib`，因此不继承旧版
monitor 的这些依赖。

## 测试设计

在 `src/monitor/CMakeLists.txt` 内以显式列表维护测试源文件。为每个源文件创建
一个名称唯一的可执行目标，链接 `monitor_lib`，并使用同名 CTest 测试注册。
测试目标只在 `BUILD_TESTS=ON` 时创建。

使用一个局部 CMake 辅助函数减少重复的 `add_executable()`、
`target_link_libraries()` 和 `add_test()` 调用，但不使用 `file(GLOB)`，以确保
新增或删除测试时构建图的变化是显式、可审查的。

测试源文件中的 `#include "../xxx.h"` 将改为 `#include "xxx.h"`，与
`monitor_lib` 发布的 include 目录保持一致。不会通过复制头文件、生成兼容目录
或添加无效路径来保留旧引用。

## 顶层测试开关

顶层 `BUILD_TESTS` 继续表达“是否构建测试”。`find_package(GTest QUIET)` 的失败
不再把该选项覆盖为 `OFF`：

- `BUILD_TESTS=OFF`：不构建任何测试。
- `BUILD_TESTS=ON` 且找到 GTest：构建 monitor 独立测试和现有 GTest 测试。
- `BUILD_TESTS=ON` 且未找到 GTest：构建 monitor 独立测试，跳过现有 GTest 测试，
  并输出说明信息。

顶层 `tests` 子目录仅在 `BUILD_TESTS AND GTest_FOUND` 时加入。CTest 在
`BUILD_TESTS=ON` 时启用，以便 monitor 测试始终可以注册。

## 验证

实现前以当前 `cmake --build --preset debug` 配置失败作为红灯基线。实现后执行：

1. `cmake --preset debug`，确认配置成功且在没有 GTest 时仍保留 monitor 测试。
2. `cmake --build --preset debug`，确认完整 debug 构建成功。
3. `ctest --preset debug`，确认所有已注册的 monitor 测试通过。
4. 检查 Git diff，确认只包含设计范围内的 CMake、测试 include 和规格变更。

如果编译或测试暴露 monitor 实现自身的问题，将先报告具体失败，不把运行逻辑
修复混入本次构建接入变更。
