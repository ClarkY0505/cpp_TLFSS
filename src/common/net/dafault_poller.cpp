#include "common/net/epoll_poller.h"
#include "common/net/poller.h"

#include <cstdlib>

namespace TLSS::NET {
/* Poller* Poller::new_default_poller(EventLoop* loop) */
std::unique_ptr<Poller> Poller::new_default_poller(EventLoop* loop) {
  if (::getenv("TLSS_USE_POLL") != nullptr) {
    return nullptr;  // 生成poll的实例
  }
  /* return new EpollPoller(loop);  // 生成epoll的实例 */
  return std::make_unique<EpollPoller>(loop);  // 生成epoll的实例
}
}  // namespace TLSS::NET
