# C++ Toy-level Storage Service Design Document

**English** | [简体中文](README.md)

## 1. Introduction

A Toy-Level File Storage Service Sys(Hereinafter referred to as ”TLFSS“) that comprises four core modules:

- **FTP Service**: A toy-level file transfer service that supports FTPS encryption and resume from interruption.
- **Proxy Gateway**: Request for Forwarding and Load Balancing
- **Monitoring System**: Detailed Indicator Collection, Anomaly Alerting, and Log Output
- **RPC Service Layer**: A gRPC-based inter-module communication framework that supports distributed deployment

Target Platform: Linux(Ubuntu 22.04.5 LTS) | Build: CMake | RPC Framework : gRPC | Language: C++17/20

## 2.Architecture

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

## 3. Module 1: FTP Server

### 3.1 Feature list

| Functional Classification | Specific functions             | FTP command |
| ------------------------- | ------------------------------ | ----------- |
| Basic transmission        | Upload File                    | STOR        |
| Basic transmission        | Download file                  | RETR        |
| Basic transmission        | File List                      | LIST, NLST  |
| File Management           | Delete file                    | DELE        |
| File Management           | Rename file                    | RNFR, RNTO  |
| Directory Management      | Create directory               | MKD         |
| Directory Management      | Delete directory               | RMD         |
| Directory Management      | Change directory               | CWD, CDUP   |
| Directory Management      | Display the current directory  | PWD         |
| TLFSS characteristics     | FTPS encryption                | AUTH TLS    |
| TLFSS characteristics     | Resuming interrupted downloads | REST, APPE  |
| Session Management        | Sign In                        | USER, PASS  |
| Session Management        | Sign Up                        | REGI,REG    |
| Session Management        | Log out                        | QUIT        |

```cpp
// Command processor
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
    void cmd_rest(const std::string& offset);  // Resuming interrupted downloads
    void cmd_auth_tls();  // FTPS

// Resuming interrupted downloads
    // File reading that supports "REST" commands
    std::ifstream open_for_read(const std::string& path, size_t offset);
    // Supports appending to the "APPE" command.
    std::ofstream open_for_append(const std::string& path);
    // Supports "REST+STOR" breakpoint upload
    std::ofstream open_for_write(const std::string& path, size_t offset);
```

## 4. Module 2: Proxy Gateway

### 4.1 Functional design

- **Load Balancing Strategy**: Round Robin, Weighted Round Robin, Least Connections, IP Hash
- **Request Forwarding**: Transparent proxy FTP control connection and data connection
- **Connection Pool Management**: Reuse backend FTP connections to reduce connection overhead
- **Health Check**: Regularly check the status of backend servers

```cpp
    // FTP Protocol Proxy Details
	void proxy_pasv();   // Passive mode requires modification of the returned IP/Port.
    void proxy_port();   // Active mode requires the establishment of a data channel proxy.
    void proxy_data_transfer();  // Data transfer agent
```

## 5. Module 3: Monitoring System

### 5.1 Monitoring Indicators

| Indicator Categories    | Specific Indicators                    | Sampling Frequency |
| ----------------------- | -------------------------------------- | ------------------ |
| Connectivity metrics    | Current number of connections          | Real time          |
| Connectivity metrics    | Total number of historical connections | Grand total        |
| Connectivity metrics    | Active sessions                        | Real time          |
| Transmission Indicators | Upload speed (bytes/s)                 | 1s                 |
| Transmission Indicators | Download speed (bytes/s)               | 1s                 |
| Transmission Indicators | Total number of bytes transferred      | Grand total        |
| Storage metrics         | Total disk capacity                    | 60s                |
| Storage metrics         | Disk space used                        | 60s                |
| Storage metrics         | Disk usage                             | 60s                |
| Operation Log           | User Login/Logout                      | Event-driven       |
| Operation Log           | File upload/download                   | Event-driven       |
| Operation Log           | File deletion/renaming                 | Event-driven       |
| Error tracking          | Number of authentication failures      | Event-driven       |
| Error tracking          | Number of transmission errors          | Event-driven       |
| Error tracking          | System Error Details                   | Event-driven       |
| User behavior           | User operation statistics              | Rollup             |
| User behavior           | Popular Files Ranking                  | Rollup             |

### 5.2 **Alerting Rule**

```cpp
struct AlertRule {
    std::string name;
    std::string metric;
    AlertCondition condition;  // >, <, ==, >=, <=
    double threshold;
    AlertSeverity severity;    // Info, Warning, Critical
    std::chrono::seconds cooldown;
};

// Predefined Alert Rules
const std::vector<AlertRule> DEFAULT_RULES = {
    {"disk_full", "disk_usage_percent", Greater, 90.0, Critical, 300s},
    {"disk_warning", "disk_usage_percent", Greater, 80.0, Warning, 600s},
    {"high_connections", "current_connections", Greater, 1000, Warning, 60s},
    {"connection_limit", "current_connections", Greater, 5000, Critical, 30s},
    {"auth_failure_spike", "auth_failures_per_min", Greater, 50, Critical, 60s},
    {"transfer_error_rate", "error_rate_percent", Greater, 5.0, Warning, 300s},
};
```

## 6. Module 4: RPC Service Layer



## 7. **Common Module**

### 7.1 **Network Infrastructure**

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

### 7.2 Thread Pool

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

### 7.3 **Configuration Management**

Read the configuration file (based) on the YAML file

## 8. **Dependency**

| Dependency | **Purpose**                         | **Must-have** |
| ---------- | ----------------------------------- | ------------- |
| gRPC       | RPC Framework                       | Yes           |
| Protobuf   | Serialization / Protocol Definition | Yes           |
| OpenSSL    | FTPS/TLS Encryption                 | Yes           |
| yaml-cpp   | Configuration File Parsing          | Yes           |
| pthread    | Multi-threaded Support              | Yes           |
| GTest      | Unit Tests (Optional)               | NO            |

---

## 9. **Expected Outcomes**

1. Standalone FTP server supporting toy features
2. High-performance proxy gateway supporting multiple load balancing strategies

3. Complete monitoring system with real-time metric collection and anomaly alerts

4. gRPC-based service communication layer supporting distributed deployment

5. Complete Proto protocol definition for easy integration by multi-language clients

6. gRPC interceptor enables unified processing of authentication, logging, and monitoring

7. Comprehensive logging support for operation auditing

---

## 10. **Progress**

- Implement Common Basic Modules

- 1.1: (TODO) Implement socket.h/cpp, including the bind/listen/accept/connect/read/write/read_line/write_line methods of the Socket class.

- 1.2: (TODO) Implement thread_pool.h/cpp, including the submit task submission and worker thread management of the thread pool.

- 1.3: (TODO) Implement io_context.h/cpp, including the run/stop/post methods of the event loop.

- 1.4: (TODO) Implement config.h/cpp, including the FTPServerConfig/GatewayConfig/MonitorConfig structures and YAML loading/saving.

- 1.5: (TODO) Implement the reactor multi-reactive + thread pool model (using a third-party library is better in a real environment). In this project, I chose to implement it myself to understand and master this model.


2. Implement File Storage Service (FTP)

- 2.1: (TODO) A simple implementation: start the server, then start the client. The client can then use command prompt (cmd) to perform file processing on the server.

- 2.2: (TODO) Implement authentication functionality

- 2.3: (TODO) 

- 2.4: (TODO) 

- 2.5: (TODO) 

## 11.Finally

- This is just the beginning of my learning a framework design. It records my technical iterations. The ultimate goal of this project is to learn the design of a complete microservice architecture and the understanding of middleware.
- Initially, I wanted to design a key-value storage service. After continuous technology selection, I found that my personal skills were insufficient to support the completion of a Redis-like service, so I settled for a file storage service.

- This project will continue to be updated as expected.
- Lose yourself to find yourself
