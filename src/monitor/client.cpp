#include "client.h"
#include "engine.h"
#include "engine_type.h"
#include "timer_types.h"

/* #include <asm-generic/socket.h> */
#include <chrono>
#include <cerrno>
/* #include <condition_variable> */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <arpa/inet.h>
#include <memory>
#include <netinet/in.h>
#include <new>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>


using namespace std::chrono_literals;

constexpr std::uint16_t Port = 9000;
constexpr int ListenBacklog = 5;

constexpr auto HeartbeatInterval = 1s;
constexpr auto SlowTimerInterval = 1500ms;
constexpr auto SlowWorkDuration = 400ms;
constexpr auto ShutdownDelay = 6s;

namespace Client{
int make_listener(std::uint16_t port){
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0){
        const int error = errno;
        std::cerr << "[server] socket failed: "
            << std::strerror(error)
            << '\n';
        return -1;
    }

    int reuse_addr = 1;
    if(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0){
        const int error = errno;
        ::close(listen_fd); std::cerr
            << "[server] setsockopt(SO_REUSEADDR) failed: "
            << std::strerror(error)
            << '\n';
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    const int res = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if(res == 0){
        ::close(listen_fd);
        std::cerr << "[server] invalid IPv4 address\n";
        return -1;
    }
    if(res < 0){
        const int error = errno;
        ::close(listen_fd);
        std::cerr
            << "[server] inet_pton failed: "
            << std::strerror(error)
            << '\n';

        return -1;
    }

    if(::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0){
        const int error = errno;
        ::close(listen_fd);

        std::cerr
            << "[server] bind failed: "
            << std::strerror(error)
            << '\n';

        return -1;
    }

    if(::listen(listen_fd, ListenBacklog) < 0){
        const int error = errno;
        ::close(listen_fd);

        std::cerr
            << "[server] listen failed: "
            << std::strerror(error)
            << '\n';

        return -1;
    }

    return listen_fd;
}
}

class SocketOwner {
public:
    explicit SocketOwner(int fd = -1) noexcept
        : _fd(fd)
        {
        }

    ~SocketOwner()
    {
        reset();
    }

    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;

    int get() const noexcept
    {
        return _fd;
    }

    void reset() noexcept
    {
        if (_fd >= 0) {
            (void)::close(_fd);
            _fd = -1;
        }
    }

private:
    int _fd{-1};
};


int register_heartbeat(TLSSMON::Engine& engine, int& heartbeat_count){
    auto heartbeat_cb = [&heartbeat_count]() -> int {
        ++heartbeat_count;
        std::cout
            << "  [heartbeat] tick #"
            << heartbeat_count
            << '\n'
            << std::flush;
        return 0;
    };

    const auto heartbeat_handle = engine.set_timer(TLSSMON::MonCallback{"heartbeat", 
                                                   heartbeat_cb, 
                                                   false}, 
                                                   TLSSMON::TimerFlags::RECURRING, 
                                                   HeartbeatInterval);

    if(!heartbeat_handle){
        std::cerr
            << "[demo] failed to register heartbeat timer\n";

        return 1;
    }
    return 0;
}

int register_slow(TLSSMON::Engine& engine){
    auto slow_timer_cb = [] {
        std::cout
            << "  [slow] start "
            << "(400ms work on worker thread)\n";

        std::this_thread::sleep_for(SlowWorkDuration);
        std::cout << "  [slow] done\n";
        return 0;
    };

    const auto slow_timer = engine.set_timer(TLSSMON::MonCallback{"slow", slow_timer_cb, false},
                                             TLSSMON::TimerFlags::RECURRING | TLSSMON::TimerFlags::WORKER,
                                             SlowTimerInterval);
    if(!slow_timer){
        std::cerr
            << "[demo] failed to register slow timer\n";
        return 1;
    }
    return 0;
}

void disconnect(TLSSMON::Engine& engine, const std::shared_ptr<ClientSession>& session){
    if(!session || session->fd < 0){
        return;
    }

    const int disconnected_fd = session->fd;
    const auto handle = session->handle;
    const bool remove_res = handle && engine.remove_aio(*handle);
    if(!remove_res && engine.get_phase() == TLSSMON::EnginePhase::RUNNING){
        engine.stop();
    }

    session->close_fd();
    std::cout
        << "  [client "
        << disconnected_fd
        << "] disconnected\n"
        << std::flush;
}

int register_client(TLSSMON::Engine& engine, int client_fd){
    if(client_fd < 0){
        return 1;
    }

    std::shared_ptr<ClientSession> session;
    try{
        session = std::make_shared<ClientSession>();
    }catch(const std::bad_alloc&){
        ::close(client_fd);
        return 1;
    }
    session->fd = client_fd;

    auto client_cb = [&engine, session]() -> int {
        char buffer[256];
        ssize_t count = -1; 
        do{
            count = ::read(session->fd, buffer, sizeof(buffer) - 1);
        }while(count < 0 && errno == EINTR);

        if(count == 0){
            disconnect(engine, session);
            return 0;
        }

        if(count < 0){
            const int error = errno;
            std::cerr
                << "  [client "
                << session->fd
                << "] read failed: "
                << std::strerror(error)
                << '\n';
            disconnect(engine, session);
            return 0;
        }
        buffer[count] = '\0';
        std::string message(buffer, static_cast<std::size_t>(count));
        const std::size_t newline = message.find_first_of("\r\n");
        if(newline != std::string::npos){
            message.erase(newline);
        }

        std::cout
            << "  [client "
            << session->fd
            << "] received: \""
            << message
            << "\"\n"
            << std::flush;

        const std::string response =
            "echo: " + message + '\n';

        ssize_t sent = -1;
        do{
            sent = ::send(session->fd, response.data(), response.size(), MSG_NOSIGNAL);
        }while(sent < 0 && errno == EINTR);

        if(sent < 0){
            const int error = errno;
            std::cerr
                << "  [client "
                << session->fd
                << "] send failed: "
                << std::strerror(error)
                << '\n';
            disconnect(engine, session);
        }
        return 0;
    };
    const auto client_handle = engine.add_aio(session->fd, 
                                              TLSSMON::MonCallback{"client-" + std::to_string(client_fd), client_cb, false});
    if(!client_handle){
        return 1;
    }

    session->handle = *client_handle;
    return 0;
}

TLSSMON::MonCallback make_accept_callback(TLSSMON::Engine& engine, int listen_fd){
    auto accept_cb = [&engine, listen_fd]() -> int{
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int client_fd = -1;
        do{
            client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        }while(client_fd < 0 && errno == EINTR);
        if(client_fd < 0){
            const int error = errno;
            if (error == EAGAIN
                || error == EWOULDBLOCK) {
                return 0;
            }

            std::cerr
                << "[server] accept failed: "
                << std::strerror(error)
                << '\n';

            return 0;
        }
        char peer_addr[INET_ADDRSTRLEN]{};
        const char* converted = ::inet_ntop(AF_INET, &peer.sin_addr, peer_addr, sizeof(peer_addr));
        if(converted == nullptr){
            std::strncpy(
                         peer_addr,
                         "<unknown>",
                         sizeof(peer_addr) - 1);
        }

        std::cout
            << "  [server] accepted connection from "
            << peer_addr
            << ':'
            << ntohs(peer.sin_port)
            << " (fd "
            << client_fd
            << ")\n"
            << std::flush;
        if (register_client(engine, client_fd) != 0) {
            std::cerr
                << "[server] failed to register client fd "
                << client_fd
                << '\n';
        }

        return 0;
    };

    return TLSSMON::MonCallback{"accept", accept_cb, false};
}
int register_shutdown_timer(
                            TLSSMON::Engine& engine)
{
    auto shutdown_callback =
        [&engine]() -> int {
            std::cout
                << "  [shutdown] stopping...\n"
                << std::flush;

            engine.stop();
            return 0;
        };

    const auto shutdown_handle =
        engine.set_timer(
                         TLSSMON::MonCallback{
                         "shutdown",
                         std::move(shutdown_callback),
                         false
                         },
                         TLSSMON::TimerFlags::ONCE,
                         ShutdownDelay);

    if (!shutdown_handle) {
        std::cerr
            << "[demo] failed to register shutdown timer\n";

        return 1;
    }

    return 0;
}


int main(){
    TLSSMON::Engine engine(TLSSMON::MonConfig{"demo", Port, 1});
    const TLSSMON::ENGINESTATE res = engine.init();

    if(res != TLSSMON::ENGINESTATE::SUCCESSFUL){
        std::cerr
            << "[demo] Engine initialization failed: "
            << static_cast<int>(res)
            << '\n';

        return 1;
    }
    if (engine.get_phase()
        != TLSSMON::EnginePhase::READY) {
        std::cerr
            << "[demo] Engine did not enter READY\n";

        return 1;
    }

    std::cout
        << "[demo] Engine initialized and ready\n";

    int heartbeat_count = 0;
    if(register_heartbeat(engine, heartbeat_count) != 0){
        return 1;
    }

    if(register_slow(engine) != 0){
        return 1;
    }
    SocketOwner listener{Client::make_listener(Port)};
    if (listener.get() < 0) {
        std::cerr
            << "[demo] failed to create listener\n";

        return 1;
    }
    const TLSSMON::MonCallback accept_cb = make_accept_callback(engine, listener.get());
    const auto listen_handle = engine.add_aio(listener.get(),accept_cb);
    if(!listen_handle){
        std::cerr
            << "[demo] failed to register listen fd\n";

        return 1;
    }
    std::cout
        << "[demo] listening on 127.0.0.1:"
        << Port
        << '\n'
        << std::flush;

    if (register_shutdown_timer(engine) != 0) {
        return 1;
    }

    std::cout
        << "[demo] running: heartbeat every "
        << HeartbeatInterval.count()
        << "ms, slow worker every "
        << SlowTimerInterval.count()
        << "ms\n"
        << "[demo] TCP server: 127.0.0.1:"
        << Port
        << '\n'
        << "[demo] connect with: nc 127.0.0.1 "
        << Port
        << '\n'
        << "[demo] automatic shutdown after "
        << ShutdownDelay.count()
        << "ms\n"
        << std::flush;

    const TLSSMON::ENGINESTATE run_result = engine.run();
    listener.reset();

    if (run_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
        std::cerr
            << "[demo] Engine stopped with error: "
            << static_cast<int>(run_result)
            << '\n';

        return 1;
    }

    std::cout
        << "[demo] Engine stopped successfully\n"
        << std::flush;

    return 0;
}
