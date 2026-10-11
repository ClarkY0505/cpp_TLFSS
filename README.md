# TLFSS：C++ 文件存储服务实验项目

**简体中文** | [English](README.es.md)

TLFSS 是一个以文件存储服务为目标的 C++17 学习项目。仓库已经实现 TCP 事件循环、线程池、MySQL 连接池、日志、内存拷贝库，以及可独立运行的监控子系统；文件传输、网关和 gRPC 服务仍在开发中。本文以当前源码为准，区分可运行功能与设计中的功能。

## 当前能做什么

| 部分 | 当前状态 | 代码入口 |
| --- | --- | --- |
| 主服务 | 在 `9000` 端口监听 TCP 连接；按 `\r\n` 分隔命令，回显输入；收到 `q` 后关闭该连接。支持 `SIGINT`/`SIGTERM` 触发有序停机 | `src/main.cpp`、`src/ftp/server.cpp` |
| 网络与基础设施 | 基于 epoll 的 `EventLoop`、`TcpServer`、连接缓冲区、IO 线程池；另有业务线程池和 MySQL 连接池 | `src/common/net/`、`src/common/base/` |
| 监控 | 独立的指标存储、模块注册、定时器、策略、CLI、UDP 指标传输及可靠 TCP 告警传输；提供独立演示程序和测试 | `src/monitor/` |
| 内存拷贝 | `TLSS::MEMORY::memcpy` 共享库，包含运行时后端选择与汇编实现 | `src/common/memory/` |
| 网关与 RPC | 编译了网关、RPC 占位代码及四份 Protobuf/gRPC 契约；目前没有从主程序启动的网关或 gRPC 服务 | `src/gateway/`、`src/rpc/`、`proto/` |

**范围说明：** 主服务现在是命令回显原型，不是完整 FTP 服务器；`USER`、`STOR`、`RETR`、`LIST`、`AUTH TLS`、`REST` 等 FTP 命令尚未接入。监控演示程序也没有接入主服务的运行链路。

## 代码结构与调用关系

```text
src/main.cpp
  └─ StorageService (src/ftp/server.cpp)
       ├─ TcpServer → EventLoop / epoll → TcpConnection
       ├─ ThreadPool（业务线程池，当前命令处理尚未派发任务）
       └─ DbConnectionPool（启动时建立 MySQL 连接）

src/monitor/demo/*（独立可执行程序）
  └─ Engine → Timer / AIO → MonitorReporter → MonitorStore
                              ├─ UDP 指标发布
                              └─ 可靠告警发布 → 本地 spool → TCP collector
```

连接建立后，主服务发送 `tlss>` 提示符。收到完整的 CRLF 命令时，`StorageService::on_message` 回显命令并再次发送提示符。停机时先停止接受连接、排空业务任务，再关闭现有连接、回收 IO 线程和数据库连接池，最后退出主事件循环。

监控子系统有自己的 `TLSSMON::Engine` 生命周期。演示程序展示模块注册、采样、策略判定、上报、传输和 CLI 查询；其中 `demo-m9-node` 与 `demo-m9-collector` 组成一个独立的双进程示例。

| 路径 | 用途 |
| --- | --- |
| `inc/`、`src/` | 按 `common`、`ftp`、`gateway`、`logger`、`monitor`、`rpc` 分组的头文件与实现 |
| `proto/` | 存储、认证、监控、网关的协议定义；生成文件放在 `build/generated/` |
| `tests/`、`src/monitor/test/` | 主服务/基础模块测试，以及监控子系统测试 |
| `config/`、`sql/` | MySQL 连接配置和建表脚本；`config/config.yaml.example` 目前为空，主程序不读取 YAML |
| `scripts/build.sh` | 配置、编译、测试的封装脚本 |
| `third/`、`third_party/` | 仓库内依赖与上游子模块；不要提交其构建产物 |
| `doc/` | 网络和内存拷贝子系统的补充资料 |

## 构建与运行

### 环境要求

- Linux；C++17 编译器、CMake 3.21+、Make、NASM、pkg-config。
- OpenSSL、Protobuf（含 `protoc`）、gRPC（含 `grpc_cpp_plugin`）、yaml-cpp、pthread；GoogleTest 用于部分可选测试。
- CMake 构建使用 `third/mysql/inc` 和 `third/mysql/lib/libmysqlclient.so` 中的 MySQL 客户端文件。运行主服务还需要可连接的 MySQL 8.0 实例（建库脚本使用 MySQL 8.0 排序规则）。

项目的 gRPC/Protobuf 上游仓库以 Git 子模块记录；按需使用 `git submodule update --init --recursive` 获取源码。CMake 当前优先查找已安装的 Protobuf 和 gRPC，而不是自动编译子模块。

### 配置数据库

主程序构造 `StorageService` 时会初始化 MySQL 连接池，并连接固定的 `tlss_db` 数据库。先在本地 MySQL 中执行建库脚本，再按本机环境调整配置：

```bash
mysql -u root -p < sql/init_tlss_db.sql
```

连接池从**可执行文件所在目录的上一级**读取 `config/mysql.cnf`。按默认输出布局，即 `bin/storage-service` 对应仓库根目录的 `config/mysql.cnf`。配置采用无空格的 `key=value` 格式，主要字段是 `ip`、`port`、`username`、`password`、`initSize`、`maxSize`；不要将真实凭据提交到仓库。启动时无法读取配置或建立初始数据库连接会导致进程退出。

### 编译、测试、启动

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./bin/storage-service
```

建议从仓库根目录启动，日志会写到相对路径 `logs/`。主程序当前不解析命令行参数，监听端口固定为 `9000`。可以用另一个终端做最小连通性检查：

```bash
printf 'hello\r\nq\r\n' | nc 127.0.0.1 9000
```

服务收到 `SIGINT` 或 `SIGTERM` 后会尝试有序停机。`scripts/build.sh -t Debug` 也可完成配置、构建和测试；`-n` 跳过测试，`-c` 会清理 `build/`、`bin/`、`lib/`，使用前请确认这些目录中的本地文件可以删除。优化构建使用 `cmake --preset release && cmake --build --preset release`。两个 preset 共用 `build/`，切换构建类型时需要重新配置。

监控演示程序输出到 `bin/dome/`，例如 `./bin/dome/demo-m9-policy` 可单独演示三种监控策略。`demo-m9-collector` 和 `demo-m9-node` 是独立示例，前者默认监听本机 UDP `9300`（指标）、TCP `9400`（告警）和 TCP `9310`（CLI）；后者需要指定模块、源 ID、outbox 目录和 CLI 端口。详情见 `src/monitor/demo/`。

## 测试与开发入口

`ctest --preset debug` 运行 CMake 注册的测试。`tests/` 包含 TCP 停机、内存拷贝链接及可选的 FTP/定时器 GoogleTest 测试；`src/monitor/test/` 覆盖监控引擎、策略、协议、存储、CLI 与可靠告警等。若未找到 GoogleTest，仅跳过依赖它的测试；其他测试仍会注册。

建议按以下顺序阅读源码：

1. `src/main.cpp` → `src/ftp/server.cpp`：了解当前可运行入口与停机流程。
2. `src/common/net/event_loop.cpp` → `src/common/net/tcp_server.cpp` → `src/common/net/tcp_connection.cpp`：追踪连接与消息处理。
3. `src/common/base/tlss_thread_pool.cpp`、`src/common/base/mysql_connection_pool.cpp`：查看任务与数据库资源的生命周期。
4. `src/monitor/demo/demo-m9-node.cpp` → `src/monitor/engine.cpp` → `src/monitor/monitor_reporter.cpp` → `src/monitor/demo/demo-m9-collector.cpp`：追踪独立监控示例。
5. `proto/`、`src/rpc/`、`src/gateway/`：了解协议规划与尚待实现的服务边界。

本项目采用 Apache License 2.0，见 [LICENSE](LICENSE)。
