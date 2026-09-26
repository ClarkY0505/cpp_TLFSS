#ifndef __INC_COMMON_CHANNEL_H__
#define __INC_COMMON_CHANNEL_H__
#include <sys/stat.h>
#include <algorithm>
#include <functional>
#include <memory>
#include "NoCopy.h"
#include "timestamp.h"

class EventLoop;

namespace TLSS::NET {
//
// EventLoop、Channel、Poller之间的关系
// 对应了 Reactor模型的Demultiplex
class Channel : NoCopy {
 public:
  using EventCallback = std::function<void()>;
  using ReadEventCallback = std::function<void(TLSS::TIME::Timestamp)>;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  //
  // fd得到poller通知以后，处理事件
  void handle_event(TLSS::TIME::Timestamp recevie_time);
  // 设置回调函数对象
  void set_read_cb(ReadEventCallback cb) {
    _read_callback = std::move(cb);
  }
  void set_write_cb(EventCallback cb) {
    _write_callback = std::move(cb);
  }
  void set_close_cb(EventCallback cb) {
    _close_callback = std::move(cb);
  }
  void set_error_cb(EventCallback cb) {
    _error_callback = std::move(cb);
  }

  // 防止channel被手动remove, channel还在执行回调操作
  void tie(const std::shared_ptr<void>&);
  int fd() const {
    return _fd;
  }

  void set_revents(int revt) {
    _revents = revt;
  }

  // 设置fd相应的事件状态
  void enable_reading() {
    _events |= _k_read_event;
    update();
  }
  void disable_reading() {
    _events &= ~_k_read_event;
    update();
  }
  void enable_writing() {
    _events |= _k_write_event;
    update();
  }

  void disable_writing() {
    _events &= ~_k_write_event;
    update();
  }
  void disable_all() {
    _events = _k_none_event;
    update();
  }

  // 返回fd当前的事件状态
  bool is_none_event() const {
    return _events == _k_none_event;
  }
  bool is_writing() const {
    return _events & _k_write_event;
  }
  bool is_reading() const {
    return _events & _k_read_event;
  }

  int index() {
    return _index;
  }
  void set_index(int idx) {
    _index = idx;
  }

  EventLoop* owner_loop() {
    return _loop;
  }
  void remove();

 private:
  void update();
  void handle_event_with_guard(TLSS::TIME::Timestamp receive_time);

  static const int _k_none_event;
  static const int _k_read_event;
  static const int _k_write_event;

  EventLoop* _loop;
  const int _fd;  // fd, poller监听的对象
  int _events;    // 注册fd感兴趣的事件
  int _revents;   // poller返回的具体发生的事件
  int _index;
  //
  // 可以用于来观察该资源的生存状态，
  // 如果在使用的时候弱指针不能提升为强指针，
  // 说明资源被释放
  std::weak_ptr<void> _tie;
  bool _tied;

  ReadEventCallback _read_callback;
  EventCallback _write_callback;
  EventCallback _close_callback;
  EventCallback _error_callback;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_CHANNEL_H__
