# Monitor 测试程序输出目录设计

## 背景

项目通过顶层 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 将可执行文件输出到仓库根目录
`bin/`。因此 `storage-service` 和 13 个 monitor 测试程序目前混放在同一目录。

## 目标

- 13 个 monitor 测试程序统一输出到仓库根目录 `bin/test/`。
- `storage-service` 和其他非测试程序继续输出到 `bin/`。
- 保持现有测试 target 名称、CTest 注册方式、链接依赖和编译选项不变。
- 移除 `bin/` 下旧位置的 13 个 monitor 测试构建产物。

## 非目标

- 不改变静态库或共享库输出目录。
- 不移动 `storage-service` 或其他非 monitor 测试程序。
- 不修改测试逻辑、monitor 运行逻辑或公共 API。
- 不重构全局输出目录策略。

## 设计

在 `src/monitor/CMakeLists.txt` 的 `add_monitor_test(target source)` 辅助函数中，
为每个测试 target 设置：

```cmake
set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test"
)
```

项目当前的 debug/release preset 均使用单配置 `Unix Makefiles` 生成器，因此该
属性会把测试程序直接写入 `${CMAKE_SOURCE_DIR}/bin/test`，不会增加额外的
`Debug` 或 `Release` 子目录。

`add_test(NAME ${target} COMMAND ${target})` 继续使用 CMake target 名称解析测试
程序的实际路径，无需硬编码 `bin/test` 路径。

不修改顶层 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`，避免把 `storage-service` 一并移动。
不在 13 个调用点重复设置属性，确保后续通过该辅助函数添加的 monitor 测试自动
遵循同一输出约定。

## 旧构建产物

完成新位置构建并确认 13 个新测试程序存在后，删除 `bin/` 根目录中名称与当前
13 个 monitor 测试 target 完全匹配的旧二进制。删除范围不包含 `bin/monitor`、
`bin/storage-service` 或其他文件；这些测试程序可随时通过 CMake 重新生成。

## 验证

实现前确认当前测试程序位于 `bin/monitor_*_test`，且 `bin/test` 中没有完整的
13 个程序，作为红灯基线。

实现后执行：

1. `cmake --preset debug`。
2. `cmake --build --preset debug`。
3. 确认 `bin/test/` 下恰好存在 13 个预期 monitor 测试可执行文件。
4. 确认 `bin/storage-service` 仍存在。
5. `ctest --preset debug`，确认 13/13 测试通过。
6. 清理旧二进制后，确认 `bin/` 根目录不再存在这 13 个测试程序。
7. 检查 Git diff 和状态，确认没有修改范围外文件。
