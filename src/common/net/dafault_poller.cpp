#include "common/net/poller.h"

#include <cstdlib>

namespace TLSS::NET {
Poller* Poller::new_default_poller([[maybe_unused]] EventLoop* loop) {
  if (::getenv("TLSS_USE_POLL")) {
    return nullptr; // 生成poll的实例
  } else {
    return nullptr; // 生成epoll的实例
  }
}
}  // namespace TLSS::NET
