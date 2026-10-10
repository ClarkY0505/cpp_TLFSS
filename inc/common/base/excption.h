#ifndef __INC_COMMON_BASE_EXCPTION_H__
#define __INC_COMMON_BASE_EXCPTION_H__
#include <exception>
#include <string>
#include <utility>
#include "common/base/current_thread.h"
namespace TLSS {
class Exception : public std::exception {
 public:
  Exception(std::string what)
      : _message(std::move(what))
      , _stack(BASE::CurrentThread::stack_trace(false)) {}
  ~Exception() noexcept override = default;

  // 使用默认的拷贝构造函数和赋值运算符即可。

  const char* what() const noexcept override {
    return _message.c_str();
  }

  const char* stack_race() const noexcept {
    return _stack.c_str();
  }

 private:
  std::string _message;
  std::string _stack;
};
}  // namespace TLSS

#endif  // __INC_COMMON_BASE_EXCPTION_H__
