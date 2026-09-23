#include "common/timestamp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace TLSS::TIME {

Timestamp::Timestamp() : _micro_seconds_since_epoch(0) {}

Timestamp::Timestamp(int64_t micro_seconds_since_epoch)
    : _micro_seconds_since_epoch(micro_seconds_since_epoch) {}

Timestamp Timestamp::now() {
  return Timestamp(time(nullptr));
}

std::string Timestamp::to_string() const {
  char buffer[128] = {0};
  tm* tm_time = localtime(&_micro_seconds_since_epoch);
  snprintf(buffer, 128, "%4d/%02d/%02d %02d:%02d:%02d", tm_time->tm_year + 1900,
           tm_time->tm_mon + 1, tm_time->tm_mday, tm_time->tm_hour, tm_time->tm_min,
           tm_time->tm_sec);
  return buffer;
}

}  // namespace TLSSTIME


/* #include <iostream> */
/* int main(){ */
/*     std::cout << TLSSTIME::Timestamp::now().to_string() << std::endl; */
/*     return 0; */
/* } */
