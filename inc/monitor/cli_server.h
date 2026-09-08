#ifndef __CLI_SERVER_H__
#define __CLI_SERVER_H__

#include "aio_types.h"
#include "cli_registry.h"
#include "cli_types.h"
#include "engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>

namespace TLSSMON {
struct CliServerConfig {
  std::uint16_t _port;
  std::size_t _max_clients{CLI_MAX_CLIENTS};
  std::string _prompt_name{"monitor"};
};

/*
 * 提供了一个测试接口
 * 只用于测试读取fd
 *
 * */
struct CliServerTestAccess;

class CliServer final {
public:
  CliServer(Engine &engine, CliRegistry &registry, CliServerConfig config);
  ~CliServer();

  CliServer(const CliServer &) = delete;
  CliServer &operator=(const CliServer &) = delete;

  CliServer(CliServer &&) = delete;
  CliServer &operator=(CliServer &&) = delete;

  /*
   * 创建 TCP Listener 并注册到 Engine AIO。
   *
   * 成功返回 true。
   *
   * 下列情况返回 false：
   *
   * - Engine 不在 READY/RUNNING；
   * - 配置非法；
   * - 当前对象已经成功启动过；
   * - socket/bind/listen/getsockname 失败；
   * - Engine::add_aio() 失败。
   */
  bool start();
  /*
   * 停止接收新连接。
   *
   * 可以重复调用。
   *
   * AIO 的移除是延迟完成的，因此底层 fd 由 ListenerResource
   * 和 AIO 回调共同管理，直到 AIO 注册真正被销毁后才关闭。
   */
  void close() noexcept;
  /*
   * 返回当前监听端口。
   *
   * 未启动或已经调用 close() 时返回 0。
   */
  std::uint16_t bound_port() const noexcept;

private:
  /*
   * 底层 Listener fd 的共享所有者。
   *
   * Engine 的 AIO 回调持有 shared_ptr，避免 CliServer 析构后
   * 回调访问已经释放的 fd。
   */
  struct ListenerResource;
  struct ClientSession;
  struct ServerState;

  static int handle_listener_ready(
      const std::shared_ptr<ServerState> &state,
      const std::shared_ptr<ListenerResource> &listener) noexcept;
  static int
  handle_client_ready(const std::shared_ptr<ServerState> &state,
                      const std::shared_ptr<ClientSession> &session) noexcept;
  /*
   * 处理 session 输入缓冲区中的完整行。
   *
   * 返回 false 表示连接需要关闭。
   */
  static bool process_input(const std::shared_ptr<ServerState> &state,
                            const std::shared_ptr<ClientSession> &session);

  /*
   * 在非阻塞 socket 上完整发送数据。
   *
   * EAGAIN 时最多等待 100ms。
   */
  static bool send_all(const std::shared_ptr<ClientSession> &session,
                       std::string_view output) noexcept;
  static bool send_prompt(
      const std::shared_ptr<ServerState>& state,
      const std::shared_ptr<ClientSession>& session) noexcept;

  /*
   * 从活动 Session 表中移除客户端，并请求删除其 AIO。
   *
   * 不直接关闭 fd。
   */
  static void
  close_session(const std::shared_ptr<ServerState> &state,
                const std::shared_ptr<ClientSession> &session) noexcept;
  friend struct CliServerTestAccess;

  Engine &_engine;
  CliRegistry &_registry;
  CliServerConfig _config;

  std::shared_ptr<ServerState> _state;

  mutable std::mutex _mutex;

  std::shared_ptr<ListenerResource> _listener;
  std::optional<AioHandle> _listener_handle;

  /*
   * 这是 ListenerResource 中 fd 的非所有权副本，仅用于状态记录
   * 和白盒测试。真正负责关闭 fd 的是 ListenerResource。
   * 也许能用上，只是预留了这个能力
   */
  int _listener_fd{-1};
  std::uint16_t _bound_port{0};

  /*
   * 当前阶段将 CliServer 定义为一次性启动对象。
   *
   * start() 成功后，即使 close()，也不能再次 start()。
   * 如需重新启动，构造新的 CliServer。
   */
  bool _ever_started{false};
};
} // namespace TLSSMON
#endif // __CLI_SERVER_H__
