#include "ftp/object_coordinator.h"
#include <functional>
#include <mutex>
#include <string>

namespace TLSS::FileStorage{

std::mutex& ObjectCoordinator::mutex_for(const std::string& object_hash){
    const auto hash_value = std::hash<std::string>{}(object_hash);
    const auto idx = hash_value % _mutexs.size();
    return _mutexs[idx];
}

}

