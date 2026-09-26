#ifndef __TIMESTAMP_H__
#define __TIMESTAMP_H__


#include <cstdint>
#include <string>

namespace TLSS::TIME{
class Timestamp{
public:
    Timestamp();
    explicit Timestamp(int64_t micro_seconds_since_epoch);
    static Timestamp now();
    std::string to_string() const;
private:
    int64_t  _micro_seconds_since_epoch;
};
}
#endif // __TIMESTAMP_H__
