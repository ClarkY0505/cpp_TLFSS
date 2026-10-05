#ifndef __INC_COMMON_NET_CALLBACKS_H__
#define __INC_COMMON_NET_CALLBACKS_H__

#include <cstddef>
#include <functional>
#include <memory>
#include "common/net/timestamp.h"
namespace TLSS::NET {
class Buffer;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCb = std::function<void(const TcpConnectionPtr&)>;
using CloseCb = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCb = std::function<void(const TcpConnectionPtr&)>;
using HighWaterMarkCb = std::function<void(const TcpConnectionPtr&, size_t)>;
using MessageCb = std::function<void(const TcpConnectionPtr&, Buffer*, TIME::Timestamp)>;
}  // namespace TLSS::NET
#endif  // __INC_COMMON_NET_CALLBACKS_H__
