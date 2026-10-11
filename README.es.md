# TLFSS: C++ File Storage Service Experiment

[简体中文](README.md) | **English**

TLFSS is a C++17 learning project aimed at building a file storage service. The repository currently contains a TCP event loop, thread pools, a MySQL connection pool, logging, a memory copy library, and a monitoring subsystem that runs independently. File transfer, the gateway, and gRPC services are still under development. This document describes the current source code and distinguishes working features from planned ones.

## What works today

| Component | Current state | Entry points |
| --- | --- | --- |
| Main service | Listens for TCP connections on port `9000`; splits commands on `\r\n`, echoes them, and closes a connection after `q`. Handles `SIGINT`/`SIGTERM` with an orderly shutdown | `src/main.cpp`, `src/ftp/server.cpp` |
| Networking and infrastructure | epoll based `EventLoop`, `TcpServer`, connection buffers, and an IO thread pool; also includes an application worker pool and a MySQL connection pool | `src/common/net/`, `src/common/base/` |
| Monitoring | Independent metric store, module registry, timers, policies, CLI, UDP metric transport, and reliable TCP alarm transport; includes standalone demos and tests | `src/monitor/` |
| Memory copy | Shared library exposing `TLSS::MEMORY::memcpy`, with runtime backend selection and assembly implementations | `src/common/memory/` |
| Gateway and RPC | Builds gateway and RPC placeholder code and generated code for four Protobuf/gRPC contracts; the main executable does not start a gateway or gRPC server | `src/gateway/`, `src/rpc/`, `proto/` |

**Current scope:** The main service is a command echo prototype, not a complete FTP server. FTP commands such as `USER`, `STOR`, `RETR`, `LIST`, `AUTH TLS`, and `REST` are not wired up. The monitoring demos are also separate from the main service.

## Code layout and execution flow

```text
src/main.cpp
  └─ StorageService (src/ftp/server.cpp)
       ├─ TcpServer → EventLoop / epoll → TcpConnection
       ├─ ThreadPool (application workers; current command handling does not submit tasks)
       └─ DbConnectionPool (opens MySQL connections during startup)

src/monitor/demo/* (standalone executables)
  └─ Engine → Timer / AIO → MonitorReporter → MonitorStore
                              ├─ UDP metric publishing
                              └─ reliable alarm publishing → local spool → TCP collector
```

After a connection opens, the main service sends a `tlss>` prompt. When `StorageService::on_message` receives a complete CRLF terminated command, it echoes the command and sends another prompt. During shutdown, it stops accepting connections, drains application tasks, closes existing connections, joins IO threads, shuts down the database pool, and exits the main event loop.

The monitoring subsystem has its own `TLSSMON::Engine` lifecycle. Its demos cover module registration, sampling, policy evaluation, reporting, transport, and CLI queries. `demo-m9-node` and `demo-m9-collector` form a separate two process example.

| Path | Purpose |
| --- | --- |
| `inc/`, `src/` | Headers and implementations grouped under `common`, `ftp`, `gateway`, `logger`, `monitor`, and `rpc` |
| `proto/` | Storage, authentication, monitoring, and gateway contracts; generated sources go into `build/generated/` |
| `tests/`, `src/monitor/test/` | Tests for the main service and infrastructure, plus monitoring subsystem tests |
| `config/`, `sql/` | MySQL connection settings and schema script; `config/config.yaml.example` is empty, and the main program does not read YAML |
| `scripts/build.sh` | Wrapper for configuration, compilation, and tests |
| `third/`, `third_party/` | Bundled dependencies and upstream submodules; do not commit their build output |
| `doc/` | Supplementary material for the networking and memory copy subsystems |

## Build and run

### Requirements

- Linux, a C++17 compiler, CMake 3.21+, Make, NASM, and pkg-config.
- OpenSSL, Protobuf (including `protoc`), gRPC (including `grpc_cpp_plugin`), yaml-cpp, and pthreads. GoogleTest is needed for some optional tests.
- The CMake build uses the MySQL client files at `third/mysql/inc` and `third/mysql/lib/libmysqlclient.so`. Running the main service also requires a reachable MySQL 8.0 instance; the schema script uses a MySQL 8.0 collation.

The upstream gRPC and Protobuf repositories are recorded as Git submodules. Run `git submodule update --init --recursive` if you need their source. CMake currently looks for installed Protobuf and gRPC packages instead of building the submodules automatically.

### Configure the database

Constructing `StorageService` initializes the MySQL connection pool, which connects to a database named `tlss_db`. Create the database and tables locally, then adjust the connection settings for your environment:

```bash
mysql -u root -p < sql/init_tlss_db.sql
```

The pool reads `config/mysql.cnf` from the **parent directory of the executable's directory**. With the default layout, `bin/storage-service` uses `config/mysql.cnf` in the repository root. The file uses `key=value` lines without surrounding spaces; key settings include `ip`, `port`, `username`, `password`, `initSize`, and `maxSize`. Do not commit real credentials. The process exits if it cannot load the configuration or open its initial database connections.

### Compile, test, and start

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./bin/storage-service
```

Start the program from the repository root so relative log files are written under `logs/`. The main program does not currently parse command line options; its listening port is fixed at `9000`. In another terminal, check the basic connection:

```bash
printf 'hello\r\nq\r\n' | nc 127.0.0.1 9000
```

The service attempts an orderly shutdown on `SIGINT` or `SIGTERM`. `scripts/build.sh -t Debug` also configures, builds, and tests the project. Use `-n` to skip tests. The `-c` option deletes `build/`, `bin/`, and `lib/`, so check those directories for local files first. For an optimized build, run `cmake --preset release && cmake --build --preset release`. Both presets share `build/`; reconfigure when switching build types.

Monitoring demos are placed in `bin/dome/`. For example, `./bin/dome/demo-m9-policy` demonstrates three monitoring policies. `demo-m9-collector` and `demo-m9-node` run separately: by default, the collector listens on local UDP `9300` for metrics, TCP `9400` for alarms, and TCP `9310` for its CLI. The node requires a module, source ID, outbox directory, and CLI port. See `src/monitor/demo/` for details.

## Tests and where to start reading

`ctest --preset debug` runs the tests registered with CMake. `tests/` covers TCP shutdown, memory copy linking, and optional GoogleTest cases for FTP and timers. `src/monitor/test/` covers the monitoring engine, policies, wire protocols, storage, CLI, and reliable alarms. If GoogleTest is unavailable, only tests that require it are skipped; the other tests are still registered.

Suggested source reading order:

1. `src/main.cpp` → `src/ftp/server.cpp`: current executable entry point and shutdown flow.
2. `src/common/net/event_loop.cpp` → `src/common/net/tcp_server.cpp` → `src/common/net/tcp_connection.cpp`: connection and message handling.
3. `src/common/base/tlss_thread_pool.cpp`, `src/common/base/mysql_connection_pool.cpp`: task and database resource lifecycles.
4. `src/monitor/demo/demo-m9-node.cpp` → `src/monitor/engine.cpp` → `src/monitor/monitor_reporter.cpp` → `src/monitor/demo/demo-m9-collector.cpp`: the independent monitoring example.
5. `proto/`, `src/rpc/`, `src/gateway/`: planned protocol boundaries and services that remain to be implemented.

This project is licensed under Apache License 2.0. See [LICENSE](LICENSE).
