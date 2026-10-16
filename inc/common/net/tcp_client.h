#ifndef __INC_COMMON_NET_TCP_CLIENT_H__
#define __INC_COMMON_NET_TCP_CLIENT_H__

#include "common/NoCopy.h"
#include "common/net/callbacks.h"
#include "common/net/event_loop.h"
#include "common/net/inet_address.h"
#include "common/net/tcp_connection.h"

#include <memory>
#include <string>

namespace TLSS::NET {
// loop 必须比客户端和外部持有的连接存活更久，退出前须处理完异步清理。
// 成员函数可从不同线程调用，但调用者必须保证 TcpClient 本体尚未开始析构。
// 析构可以与内部异步任务并发；已经开始的业务回调仍可能执行到结束。
class TcpClient : NoCopy {
public:
    TcpClient(EventLoop* loop, const InetAddress& server_addr, const std::string& name_arg);
    ~TcpClient();

    void connect();
    // 取消建连/重试，并在发送缓冲区排空后半关闭已有连接。
    void disconnect();
    // 取消建连/重试，保留已有连接；返回不代表 loop 已完成停止。
    void stop();

    TcpConnectionPtr connection() const;
    EventLoop* get_loop() const;
    const std::string& name() const;
    bool retry() const;
    void enable_retry();

    // 配置由状态锁保护；连接构造时获取快照，修改仅影响此后构造的连接。
    void set_connection_cb(const ConnectionCb& cb);
    void set_message_cb(const MessageCb& cb);
    void set_write_complete_cb(const WriteCompleteCb& cb);

private:
    // 回调持有 State 的弱引用，执行时升级为强引用，不捕获 TcpClient 裸 this。
    struct State;
    std::shared_ptr<State> _state;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_TCP_CLIENT_H__
