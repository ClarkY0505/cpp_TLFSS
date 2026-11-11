#ifndef __INC_FTP_OBJECT_COORDINATOR_H__
#define __INC_FTP_OBJECT_COORDINATOR_H__

#include <array>
#include <mutex>
#include <string>
namespace TLSS::FileStorage{
class ObjectCoordinator{
public:
    std::mutex& mutex_for(const std::string& object_hash);
private:
    std::array<std::mutex, 256> _mutexs;
};

}
#endif // __INC_FTP_OBJECT_COORDINATOR_H__
