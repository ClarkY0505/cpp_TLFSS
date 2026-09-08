#include "aio_types.h"
#include "cli_prompt.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "cli_types.h"
#include "engine.h"
#include "engine_type.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <linux/falloc.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <new>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace TLSSMON {
namespace {
constexpr int CLI_LISTEN_BACKLOG = static_cast<int>(CLI_MAX_CLIENTS);
constexpr std::size_t CLI_READ_BUFFER_SIZE = 4096U;
constexpr std::chrono::milliseconds CLI_SEND_TIMEOUT{100};
constexpr std::string_view CLI_LINE_TOO_LONG_RESPONSE{"error: line too long\n"};

constexpr bool accepts_cli_work(EnginePhase phase) noexcept {
  return phase == EnginePhase::READY || phase == EnginePhase::RUNNING;
}

/*
 * 局部 fd 所有者。
 *
 * start() 创建 Listener 的过程中发生任何失败时，
 * 都能自动关闭尚未转交的 fd。
 */
class UniqueFd final {
public:
  explicit UniqueFd(int fd = -1) noexcept : _fd(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : _fd(other.release()) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }

    return *this;
  }

  int get() const noexcept { return _fd; }

  int release() noexcept {
    const int result = _fd;
    _fd = -1;
    return result;
  }
  void reset(int fd = -1) noexcept {
    if (_fd >= 0) {
      ::close(_fd);
    }
    _fd = fd;
  }

private:
  int _fd;
};

std::string format_system_reply(std::string_view output) {
  if (output.empty()) {
    return {};
  }

  std::string formatted;
  formatted.reserve(output.size() + 32U);
  formatted += "* ";

  for (std::size_t index = 0U; index < output.size(); ++index) {
    const char character = output[index];
    formatted.push_back(character);

    /*
     * 下一行还有内容时，为下一行增加前缀。
     *
     * 如果字符串以 '\n' 结尾，不产生多余的 '*'
     * 字符。
     */
    if (character == '\n' && index + 1U < output.size()) {
      formatted += "* ";
    }
  }

  return formatted;
}

bool set_close_on_exec(int fd) noexcept {
  const int old_flags = ::fcntl(fd, F_GETFD);

  if (old_flags < 0) {
    return false;
  }

  return ::fcntl(fd, F_SETFD, old_flags | FD_CLOEXEC) == 0;
}

bool set_nonblocking(int fd) noexcept {
  const int old_flags = ::fcntl(fd, F_GETFL);
  if (old_flags < 0) {
    return false;
  }

  if ((old_flags & O_NONBLOCK) != 0) {
    return true;
  }

  return ::fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) == 0;
}

} // namespace

/*
 * Listener fd 的真正所有者。
 *
 * CliServer 和 Engine AIO 回调都可以持有该对象。
 * 只有最后一个 shared_ptr 被释放时，fd 才会关闭。
 */
struct CliServer::ListenerResource final {
  explicit ListenerResource(int fd) noexcept : _fd(fd) {}
  ~ListenerResource() {
    if (_fd >= 0) {
      ::close(_fd);
    }
  }

  ListenerResource(const ListenerResource &) = delete;
  ListenerResource &operator=(const ListenerResource &) = delete;

  const int _fd;
};

struct CliServer::ClientSession final {
  ClientSession() = default;
  ~ClientSession() {
    if (_fd >= 0) {
      ::close(_fd);
    }
  }

  int _fd{-1};
  std::mutex _registration_mutex;
  AioHandle _aio_handle{};
  CliSessionContext _context;

  std::string _input;
  std::atomic<bool> _closing{false};
};

struct CliServer::ServerState final {
  ServerState(Engine &engine, CliRegistry &registry, std::size_t max_clients,
              std::string prompt_name)
      : _engine(engine), _registry(registry), _max_clients(max_clients),
        _prompt_name(std::move(prompt_name)) {}

  Engine &_engine;
  CliRegistry &_registry;

  const std::size_t _max_clients;
  const std::string _prompt_name;
  std::atomic<bool> _closing{false};
  std::mutex _sessions_mutex;

  std::unordered_map<int, std::shared_ptr<ClientSession>> _sessions;
};

CliServer::CliServer(Engine &engine, CliRegistry &registry,
                     CliServerConfig config)
    : _engine(engine), _registry(registry), _config(config),
      _state(std::make_shared<ServerState>(
          engine, registry, config._max_clients, config._prompt_name)) {}
CliServer::~CliServer() { close(); }

bool CliServer::start() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_ever_started || _listener) {
    return false;
  }

  if (_config._max_clients == 0 || _config._max_clients > CLI_MAX_CLIENTS ||
      !is_valid_cli_prompt_name(_config._prompt_name)) {
    return false;
  }

  const EnginePhase phase = _engine.get_phase();
  if (phase != EnginePhase::READY && phase != EnginePhase::RUNNING) {
    return false;
  }

  UniqueFd socket_fd(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (socket_fd.get() < 0) {
    return false;
  }

  if (!set_close_on_exec(socket_fd.get())) {
    return false;
  }

  if (!set_nonblocking(socket_fd.get())) {
    return false;
  }

  const int reuse_address = 1;
  if (::setsockopt(socket_fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) != 0) {
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(_config._port);

  if (::bind(socket_fd.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    return false;
  }

  if (::listen(socket_fd.get(), CLI_LISTEN_BACKLOG) != 0) {
    return false;
  }

  sockaddr_in bound_address{};
  socklen_t bound_address_size = sizeof(bound_address);
  if (::getsockname(socket_fd.get(),
                    reinterpret_cast<sockaddr *>(&bound_address),
                    &bound_address_size) != 0) {
    return false;
  }

  if (bound_address_size < sizeof(sockaddr_in) ||
      bound_address.sin_family != AF_INET) {
    return false;
  }

  const std::uint16_t actual_port = ntohs(bound_address.sin_port);
  if (actual_port == 0) {
    return false;
  }

  std::shared_ptr<ListenerResource> listener;
  try {
    /*
     * make_shared() 成功前仍由 UniqueFd 持有描述符，避免内存分配
     * 失败时因为提前 release() 造成 Listener fd 泄漏。
     */
    listener = std::make_shared<ListenerResource>(socket_fd.get());
  } catch (const std::bad_alloc &) {
    return false;
  }

  /*
   * AIO 回调只捕获 ListenerResource，不捕获 this。
   *
   * 因此，即使 CliServer 析构，而 AIO 的延迟删除尚未完成，
   * 回调也不会访问已经析构的 CliServer 对象。
   */
  (void)socket_fd.release();
  const std::shared_ptr<ServerState> state = _state;

  MonCallback callback{"cli-listener",
                       [state, listener]() noexcept -> int {
                         return CliServer::handle_listener_ready(state,
                                                                 listener);
                       },
                       false};
  const std::optional<AioHandle> handle =
      _engine.add_aio(listener->_fd, std::move(callback));

  if (!handle.has_value()) {
    return false;
  }

  _listener_fd = listener->_fd;
  _listener = std::move(listener);
  _listener_handle = *handle;
  _bound_port = actual_port;
  _ever_started = true;

  return true;
}

void CliServer::close() noexcept {
  std::shared_ptr<ListenerResource> listener;
  std::optional<AioHandle> listener_handle;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_listener) {
      _listener_fd = -1;
      _bound_port = 0;
      _listener_handle.reset();
      return;
    }

    _state->_closing.store(true, std::memory_order_release);

    /*
     * 将当前持有者移动到局部变量。
     *
     * 底层 fd 不会立即关闭，因为 Engine AIO 回调仍持有
     * ListenerResource 的 shared_ptr。
     */
    listener = std::move(_listener);
    listener_handle = _listener_handle;
    _listener_handle.reset();
    _listener_fd = -1;
    _bound_port = 0;
  }
  if (listener_handle.has_value()) {
    /*
     * remove_aio() 是延迟移除,它的返回值是true/false
     * 在这个并不关心它是否真的删除，只有引擎框架才关心是否真的删除
     * 所以不判断返回值采用（void）
     *
     * 调用后 Engine 会唤醒 select()，在安全位置删除 AIO 项。
     * 如果 Engine 已进入 STOPPING/STOPPED，此调用可能返回 false；
     * 此时 Engine 的清理流程仍会销毁 AIO 回调，并最终释放 fd。
     */
    (void)_engine.remove_aio(*listener_handle);
  }

  for (;;) {
    std::shared_ptr<ClientSession> session;
    {
      std::lock_guard<std::mutex> lock(_state->_sessions_mutex);
      if (_state->_sessions.empty()) {
        break;
      }

      const auto entry = _state->_sessions.begin();
      session = entry->second;
      _state->_sessions.erase(entry);
    }

    close_session(_state, session);
  }
}

std::uint16_t CliServer::bound_port() const noexcept {
  std::lock_guard<std::mutex> lock(_mutex);
  return _bound_port;
}

int CliServer::handle_listener_ready(
    const std::shared_ptr<ServerState> &state,
    const std::shared_ptr<ListenerResource> &listener) noexcept {
  if (!state || !listener || listener->_fd < 0) {
    return 0;
  }

  try {
    for (;;) {
      sockaddr_storage peer_address{};
      socklen_t peer_address_size = sizeof(peer_address);
      UniqueFd accepted{
          ::accept4(listener->_fd, reinterpret_cast<sockaddr *>(&peer_address),
                    &peer_address_size, SOCK_NONBLOCK | SOCK_CLOEXEC)};

      if (accepted.get() >= 0) {
        if (!accepts_cli_work(state->_engine.get_phase())) {
          continue;
        }
        std::shared_ptr<ClientSession> session =
            std::make_shared<ClientSession>();
        session->_fd = accepted.release();
        bool admitted = false;
        {
          std::lock_guard<std::mutex> lock(state->_sessions_mutex);
          if (!state->_closing.load(std::memory_order_acquire) &&
              state->_sessions.size() < state->_max_clients) {
            const auto insertion =
                state->_sessions.emplace(session->_fd, session);
            admitted = insertion.second;
          }
        }

        if (!admitted) {
          continue;
        }

        std::optional<AioHandle> handle;
        {
          std::lock_guard<std::mutex> registration_lock(
              session->_registration_mutex);
          if (!session->_closing.load(std::memory_order_acquire)) {
            try {
              MonCallback callback{"cli-client",
                                   [state, session]() noexcept -> int {
                                     return CliServer::handle_client_ready(
                                         state, session);
                                   },
                                   false};

              handle =
                  state->_engine.add_aio(session->_fd, std::move(callback));
              if (handle.has_value()) {
                session->_aio_handle = *handle;
              }
            } catch (...) {
              handle.reset();
            }
          }
        }

        if (!handle.has_value()) {
          close_session(state, session);
          continue;
        }
        if (!send_prompt(state, session)) {
          close_session(state, session);
        }
        continue;
      }

      const int accept_error = errno;
      if (accept_error == EINTR) {
        continue;
      }

      if (accept_error == EAGAIN || accept_error == EWOULDBLOCK) {
        return 0;
      }

      /*
       * 单次 accept 错误不终止 Engine。
       * TODO Log 记录 accept_error。
       */
      return 0;
    }
  } catch (...) {
    /*
     * 内存分配失败这些异常不能穿透Engine AIO回调
     * TODO Log 记录异常
     * */
    return 0;
  }
}

int CliServer::handle_client_ready(
    const std::shared_ptr<ServerState> &state,
    const std::shared_ptr<ClientSession> &session) noexcept {
  if (!state || !session || session->_fd < 0) {
    return 0;
  }

  if (!accepts_cli_work(state->_engine.get_phase())) {
    close_session(state, session);
    return 0;
  }

  /*
   * 等待add_aio完成保存
   * 如果说能获取到注册锁说明已经完成保存
   * */
  {
    std::lock_guard<std::mutex> registration_lock(session->_registration_mutex);
  }

  if (session->_closing.load(std::memory_order_acquire)) {
    return 0;
  }

  try {
    char buffer[CLI_READ_BUFFER_SIZE];
    for (;;) {
      const ssize_t received =
          ::recv(session->_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (received > 0) {
        session->_input.append(buffer, static_cast<std::size_t>(received));
        if (!process_input(state, session)) {
          close_session(state, session);
          return 0;
        }

        continue;
      }
      if (received == 0) {
        close_session(state, session);
        return 0;
      }

      const int receive_error = errno;
      if (receive_error == EINTR) {
        continue;
      }

      if (receive_error == EAGAIN || receive_error == EWOULDBLOCK) {
        return 0;
      }
      close_session(state, session);
      return 0;
    }
  } catch (...) {
    /*
     * std::string 扩容或 Registry 分派过程中发生异常时，
     * TODO Log 记录上述这些异常
     */
    close_session(state, session);
    return 0;
  }
}

bool CliServer::process_input(const std::shared_ptr<ServerState> &state,
                              const std::shared_ptr<ClientSession> &session) {
  for (;;) {
    if (state->_closing.load(std::memory_order_acquire) ||
        session->_closing.load(std::memory_order_acquire) ||
        !accepts_cli_work(state->_engine.get_phase())) {
      return false;
    }
    const std::size_t newline = session->_input.find('\n');
    if (newline == std::string::npos) {
      const bool possible_crlf =
          session->_input.size() == CLI_MAX_LINE_SIZE + 1U &&
          !session->_input.empty() && session->_input.back() == '\r';

      if (session->_input.size() > CLI_MAX_LINE_SIZE && !possible_crlf) {
        send_all(session, CLI_LINE_TOO_LONG_RESPONSE);
        return false;
      }

      /*
       * 只保留半行，等待下一次 recv()。
       */
      return true;
    }

    std::size_t content_size = newline;
    if (content_size > 0U && session->_input[content_size - 1U] == '\r') {
      --content_size;
    }

    if (content_size > CLI_MAX_LINE_SIZE) {
      send_all(session, CLI_LINE_TOO_LONG_RESPONSE);
      return false;
    }

    std::string line = session->_input.substr(0U, content_size);
    session->_input.erase(0U, newline + 1U);
    if (line.empty()) {
      if (!send_prompt(state, session)) {
        return false;
      }
      continue;
    }

    const CliDispatchResult result =
        state->_registry.dispatch(line, session->_context);
    if (!result._output.empty()) {
      const std::string reply = format_system_reply(result._output);

      if (!send_all(session, reply)) {
        return false;
      }
    }

    if (!send_prompt(state, session)) {
      return false;
    }
  }
}

bool CliServer::send_prompt(
    const std::shared_ptr<ServerState> &state,
    const std::shared_ptr<ClientSession> &session) noexcept {
  if (!state || !session || session->_fd < 0) {
    return false;
  }

  if (state->_closing.load(std::memory_order_acquire) ||
      session->_closing.load(std::memory_order_acquire) ||
      !accepts_cli_work(state->_engine.get_phase())) {
    return false;
  }

  try {
    const std::string prompt =
        make_cli_prompt(state->_prompt_name, session->_context);
    if (prompt.empty()) {
      return false;
    }

    return send_all(session, prompt);
  } catch (...) {
    /*
     * make_cli_prompt() 可能因为字符串分配失败抛出异常。
     * 异常不能穿透 Engine AIO 回调。
     * TODO Log
     */
    return false;
  }
}

bool CliServer::send_all(const std::shared_ptr<ClientSession> &session,
                         std::string_view output) noexcept {
  if (!session || session->_fd < 0) {
    return false;
  }

  using Clock = std::chrono::steady_clock;

  /*
   * 整次发送共享同一个绝对截止时间。
   *
   * 短写、EINTR 和多次 EAGAIN 都不能延长这个时间。
   */
  const Clock::time_point deadline = Clock::now() + CLI_SEND_TIMEOUT;

  std::size_t offset = 0U;

  /*
   * 计算距离绝对截止时间还剩多少毫秒。
   *
   * poll() 使用整数毫秒，所以不足 1ms 时向上取整为 1ms。
   * 返回 0 表示已经到期。
   */
  const auto remaining_timeout_ms = [deadline]() noexcept -> int {
    const Clock::time_point now = Clock::now();

    if (now >= deadline) {
      return 0;
    }

    const auto remaining_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
            .count();

    /*
     * 正数毫秒向上取整：
     *
     * 1ns       → 1ms
     * 1,000,000 → 1ms
     * 1,000,001 → 2ms
     */
    constexpr std::int64_t nanoseconds_per_millisecond = INT64_C(1'000'000);

    const std::int64_t remaining_milliseconds =
        (remaining_nanoseconds + nanoseconds_per_millisecond - 1) /
        nanoseconds_per_millisecond;

    return static_cast<int>(remaining_milliseconds);
  };

  while (offset < output.size()) {
    if (session->_closing.load(std::memory_order_acquire)) {
      return false;
    }

    if (Clock::now() >= deadline) {
      return false;
    }

    const ssize_t sent =
        ::send(session->_fd, output.data() + offset, output.size() - offset,
               MSG_NOSIGNAL | MSG_DONTWAIT);

    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent == 0) {
      return false;
    }

    const int send_error = errno;

    if (send_error == EINTR) {
      continue;
    }

    if (send_error != EAGAIN && send_error != EWOULDBLOCK) {
      return false;
    }

    for (;;) {
      if (session->_closing.load(std::memory_order_acquire)) {
        return false;
      }

      const int timeout_ms = remaining_timeout_ms();

      if (timeout_ms <= 0) {
        return false;
      }

      pollfd descriptor{};
      descriptor.fd = session->_fd;
      descriptor.events = POLLOUT;

      const int poll_result = ::poll(&descriptor, 1, timeout_ms);

      if (poll_result > 0) {
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          return false;
        }

        if ((descriptor.revents & POLLOUT) == 0) {
          return false;
        }

        /*
         * socket 已经可写，退出 poll 循环，
         * 回到外层重新调用 send()。
         */
        break;
      }

      if (poll_result == 0) {
        return false;
      }

      const int poll_error = errno;

      if (poll_error == EINTR) {
        continue;
      }

      return false;
    }
  }

  return true;
}

void CliServer::close_session(
    const std::shared_ptr<ServerState> &state,
    const std::shared_ptr<ClientSession> &session) noexcept {
  if (!state || !session) {
    return;
  }

  /*
   * 无论是不是首次关闭，都尝试从活动表删除。
   *
   * 比较 shared_ptr 可以防止未来代码变化后误删同 fd 的其他对象。
   */
  {
    std::lock_guard<std::mutex> lock(state->_sessions_mutex);

    const auto entry = state->_sessions.find(session->_fd);

    if (entry != state->_sessions.end() && entry->second == session) {
      state->_sessions.erase(entry);
    }
  }

  const bool already_closing =
      session->_closing.exchange(true, std::memory_order_acq_rel);
  if (already_closing) {
    return;
  }

  AioHandle handle{};
  {
    std::lock_guard<std::mutex> registration_lock(session->_registration_mutex);
    handle = session->_aio_handle;
  }

  if (handle) {
    state->_engine.remove_aio(handle);
  }
}

} // namespace TLSSMON
