#include "common/memory/tlss_memcpy.h"

#include <array>
#include <cstddef>

int main() {
    std::array<unsigned char, 8192> source{};
    std::array<unsigned char, 8192> destination{};

    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<unsigned char>(i % 251);
    }

    if (TLSS::MEMORY::memcpy(destination.data(), source.data(), 4096) != destination.data()) {
        return 1;
    }
    for (std::size_t i = 0; i < 4096; ++i) {
        if (destination[i] != source[i]) {
            return 2;
        }
    }

    if (TLSS::MEMORY::memcpy(destination.data(), source.data(), source.size(),
                             TLSS::MEMORY::CopyBackend::RepMovsb,
                             TLSS::MEMORY::CopyHint::Default) != destination.data()) {
        return 3;
    }
    if (destination != source) {
        return 4;
    }

    return TLSS::MEMORY::memcpy(destination.data(), source.data(), 0) == destination.data()
               ? 0
               : 5;
}
