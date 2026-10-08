#ifndef __INC_COMMON_NET_CALLBACKS_H__
#define __INC_COMMON_NET_CALLBACKS_H__
#include "common/net/timestamp.h"

#include <cstddef>
#include <functional>
#include <memory>
namespace TLSS::NET {
class Buffer;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCb = std::function<void(const TcpConnectionPtr&)>;
using CloseCb = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCb = std::function<void(const TcpConnectionPtr&)>;
using HighWaterMarkCb = std::function<void(const TcpConnectionPtr&, size_t)>;
using MessageCb = std::function<void(const TcpConnectionPtr&, Buffer*, TIME::Timestamp)>;

void defaultConnectionCallback(const TcpConnectionPtr& conn);
void defaultMessageCallback(const TcpConnectionPtr& conn, Buffer* buffer,
                            TIME::Timestamp receive_time);
}  // namespace TLSS::NET
#endif  // __INC_COMMON_NET_CALLBACKS_H__
