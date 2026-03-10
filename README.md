# C++ 玩具级存储服务设计文档

## 1. 项目概述

一个玩具级文件存储服务系统，包含四大核心模块：

- **FTP服务端**：支持FTPS加密、断点续传的玩具级文件传输服务
- **代理网关**：请求转发与负载均衡
- **监控系统**：详细指标采集、异常告警、日志输出
- **RPC服务层**：基于gRPC的模块间通信框架，支持分布式部署

目标平台：Linux(Ubuntu 22.04.5 LTS) | 构建系统：CMake | RPC框架：gRPC | 语言：C++17/20

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client Requests                          │
│                    (FTP Client / gRPC Client)                   │
└─────────────────────────────────────────────────────────────────┘
                              │
          ┌───────────────────┴───────────────────┐
          ▼                                       ▼
┌──────────────────────┐              ┌──────────────────────────┐
│   FTP Protocol       │              │   gRPC API Gateway       │
│   (Port 21/990)      │              │   (Port 50051)           │
└──────────────────────┘              └──────────────────────────┘
          │                                       │
          └───────────────────┬───────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Proxy Gateway (gRPC)                       │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │Load Balancer│  │RequestRouter│  │  Connection Pool        │  │
│  │   (gRPC)    │  │   (gRPC)    │  │       (gRPC)            │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼ gRPC Calls
            ┌─────────────────┼─────────────────┐
            ▼                 ▼                 ▼
┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐
│ Storage Node #1   │ │ Storage Node #2   │ │ Storage Node #N   │
│  ┌─────────────┐  │ │  ┌─────────────┐  │ │  ┌─────────────┐  │
│  │ FTP Server  │  │ │  │ FTP Server  │  │ │  │ FTP Server  │  │
│  │ gRPC Service│  │ │  │ gRPC Service│  │ │  │ gRPC Service│  │
│  │ File Manager│  │ │  │ File Manager│  │ │  │ File Manager│  │
│  └─────────────┘  │ │  └─────────────┘  │ │  └─────────────┘  │
└───────────────────┘ └───────────────────┘ └───────────────────┘
            │                 │                 │
            └─────────────────┼─────────────────┘
                              ▼ gRPC Metrics/Logs
┌─────────────────────────────────────────────────────────────────┐
│                  Monitoring System (gRPC)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────┐  │
│  │MetricsService│  │ AlertService │  │ LogService            │  │
│  │   (gRPC)     │  │   (gRPC)     │  │   (gRPC)              │  │
│  └──────────────┘  └──────────────┘  └───────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. 模块一：FTP服务端

### 3.1 功能清单

| 功能分类 | 具体功能     | FTP命令    |
| -------- | ------------ | ---------- |
| 基础传输 | 上传文件     | STOR       |
| 基础传输 | 下载文件     | RETR       |
| 基础传输 | 文件列表     | LIST, NLST |
| 文件管理 | 删除文件     | DELE       |
| 文件管理 | 重命名文件   | RNFR, RNTO |
| 目录管理 | 创建目录     | MKD        |
| 目录管理 | 删除目录     | RMD        |
| 目录管理 | 切换目录     | CWD, CDUP  |
| 目录管理 | 显示当前目录 | PWD        |
| 玩具特性 | FTPS加密     | AUTH TLS   |
| 玩具特性 | 断点续传     | REST, APPE |
| 会话管理 | 用户认证     | USER, PASS |
| 会话管理 | 退出登录     | QUIT       |

```cpp
// 命令处理器
    void cmd_user(const std::string& username);
    void cmd_pass(const std::string& password);
    void cmd_stor(const std::string& filename);
    void cmd_retr(const std::string& filename);
    void cmd_list(const std::string& path);
    void cmd_dele(const std::string& filename);
    void cmd_rnfr(const std::string& filename);
    void cmd_rnto(const std::string& filename);
    void cmd_mkd(const std::string& dirname);
    void cmd_rmd(const std::string& dirname);
    void cmd_cwd(const std::string& path);
    void cmd_pwd();
    void cmd_rest(const std::string& offset);  // 断点续传
    void cmd_auth_tls();  // FTPS

// 断点续传
    // 支持REST命令的文件读取
    std::ifstream open_for_read(const std::string& path, size_t offset);
    // 支持APPE命令的追加写入
    std::ofstream open_for_append(const std::string& path);
    // 支持REST+STOR的断点上传
    std::ofstream open_for_write(const std::string& path, size_t offset);
```

## 4. 模块二：代理网关

### 4.1 功能设计

- **负载均衡策略**：轮询(Round Robin)、加权轮询、最少连接数、IP哈希
- **请求转发**：透明代理FTP控制连接和数据连接
- **连接池管理**：复用后端FTP连接，减少连接开销
- **健康检查**：定期检测后端服务器状态

```cpp
    // FTP协议代理细节
	void proxy_pasv();   // 被动模式需要修改返回的IP/端口
    void proxy_port();   // 主动模式需要建立数据通道代理
    void proxy_data_transfer();  // 数据传输代理
```

## 5. 模块三：监控系统

### 5.1 监控指标

| 指标类别 | 具体指标          | 采集频率 |
| -------- | ----------------- | -------- |
| 连接指标 | 当前连接数        | 实时     |
| 连接指标 | 历史连接总数      | 累计     |
| 连接指标 | 活跃会话数        | 实时     |
| 传输指标 | 上传速率(bytes/s) | 1秒      |
| 传输指标 | 下载速率(bytes/s) | 1秒      |
| 传输指标 | 总传输字节数      | 累计     |
| 存储指标 | 磁盘总容量        | 60秒     |
| 存储指标 | 磁盘已用空间      | 60秒     |
| 存储指标 | 磁盘使用率        | 60秒     |
| 操作日志 | 用户登录/登出     | 事件驱动 |
| 操作日志 | 文件上传/下载     | 事件驱动 |
| 操作日志 | 文件删除/重命名   | 事件驱动 |
| 错误追踪 | 认证失败次数      | 事件驱动 |
| 错误追踪 | 传输错误次数      | 事件驱动 |
| 错误追踪 | 系统错误详情      | 事件驱动 |
| 用户行为 | 用户操作统计      | 汇总     |
| 用户行为 | 热门文件排行      | 汇总     |

### 5.2 告警规则

```cpp
struct AlertRule {
    std::string name;
    std::string metric;
    AlertCondition condition;  // >, <, ==, >=, <=
    double threshold;
    AlertSeverity severity;    // Info, Warning, Critical
    std::chrono::seconds cooldown;
};

// 预置告警规则
const std::vector<AlertRule> DEFAULT_RULES = {
    {"disk_full", "disk_usage_percent", Greater, 90.0, Critical, 300s},
    {"disk_warning", "disk_usage_percent", Greater, 80.0, Warning, 600s},
    {"high_connections", "current_connections", Greater, 1000, Warning, 60s},
    {"connection_limit", "current_connections", Greater, 5000, Critical, 30s},
    {"auth_failure_spike", "auth_failures_per_min", Greater, 50, Critical, 60s},
    {"transfer_error_rate", "error_rate_percent", Greater, 5.0, Warning, 300s},
};
```

## 6. RPC模块



## 7. 公共模块

### 7.1 网络基础设施

```cpp
class IOContext {
public:
    void run();
    void stop();
    void post(std::function<void()> task);
};

class Socket {
public:
    Socket();
    explicit Socket(int fd);
    
    void bind(const std::string& addr, uint16_t port);
    void listen(int backlog = 128);
    Socket accept();
    void connect(const std::string& addr, uint16_t port);
    
    ssize_t read(void* buf, size_t len);
    ssize_t write(const void* buf, size_t len);
    std::string read_line();
    void write_line(const std::string& line);
    
    void set_nonblocking(bool nonblocking);
    void set_reuse_addr(bool reuse);
    void close();
    
    int fd() const { return fd_; }
    
private:
    int fd_ = -1;
};
```

### 7.2 线程池

```cpp
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();
    
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<F, Args...>>;
    
private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};
```

### 7.3 配置管理

依据yaml文件读取配置文件



## 8. 依赖项

| 依赖     | 用途             | 必需 |
| -------- | ---------------- | ---- |
| gRPC     | RPC通信框架      | 是   |
| Protobuf | 序列化/协议定义  | 是   |
| OpenSSL  | FTPS/TLS加密     | 是   |
| yaml-cpp | 配置文件解析     | 是   |
| pthread  | 多线程支持       | 是   |
| GTest    | 单元测试（可选） | 否   |

---

## 9. 预期成果

1. 可独立运行的FTP服务端，支持玩具级特性
2. 高性能代理网关，支持多种负载均衡策略
3. 完整的监控系统，实时指标采集与异常告警
4. 基于gRPC的服务通信层，支持分布式部署
5. 完整的Proto协议定义，便于多语言客户端接入
6. gRPC拦截器实现认证、日志、监控的统一处理
7. 完善的日志记录，支持操作审计

---

## 10. 进度

 1：实现公共基础模块（common）

- 1.1: TODO 实现socket.h/cpp，包含Socket类的bind/listen/accept/connect/read/write/read_line/write_line方法
- 1.2: TODO 实现thread_pool.h/cpp，包含线程池的submit任务提交和工作线程管理
- 1.3: TODO 实现io_context.h/cpp，包含事件循环run/stop/post方法
- 1.4: TODO 实现config.h/cpp，包含FTPServerConfig/GatewayConfig/MonitorConfig结构体及YAML加载/保存
- 1.5: TODO 实现reactor多反应式+线程池模型（在实际环境中使用第三方库更好），在此项目当中为了解掌握该模型选择自我实现

 2：实现文件存储服务（ftp）

- 2.1: TODO 简单实现，服务器端启动，然后启动客户端，通过客户端可以输入cmd命令进行服务器上的文件处理能力
- 2.2: TODO 实现鉴权功能
- 2.3: TODO
- 2.4: TODO
- 2.5: TODO
  
## 11.写在最后

- 这仅仅是我学习一个框架设计的开始，它记录了我技术的迭代，这个项目最终目的是学会一整套微服务架构而设计以及对于中间件的理解
- 起初想设计一个键值存储服务，在不断的对技术选型后，发现个人技术还不足以支撑完成一个类Redis的服务，故退而求其次选择了文件存储服务
- 此项目不出意外会持续更新
- 潮水成诗，破碎生长
