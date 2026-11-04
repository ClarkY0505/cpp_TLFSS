#include "ftp/verify_engine.h"

#include <stdexcept>

#include "ftp/transfer_logger.h"

namespace TLSS::FileStorage::Transfer {
Digest make_digest() {
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        transfer_logger()->error("SHA-256 initialization failed");
        throw std::runtime_error("SHA-256 initialization failed");
    }

    return digest;
}

void update_digest(EVP_MD_CTX* digest, const void* data, std::size_t size) {
    if (EVP_DigestUpdate(digest, data, size) != 1) {
        transfer_logger()->error("SHA-256 update failed");
        throw std::runtime_error("SHA-256 update failed");
    }
}

std::string finish_digest(EVP_MD_CTX* digest) {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length = 0;

    if (EVP_DigestFinal_ex(digest, bytes, &length) != 1 || length != 32) {
        transfer_logger()->error("SHA-256 finalization failed");
        throw std::runtime_error("SHA-256 finalization failed");
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string hash;
    hash.reserve(length * 2);

    for (unsigned int i = 0; i < length; ++i) {
        hash.push_back(digits[bytes[i] >> 4]);
        hash.push_back(digits[bytes[i] & 15]);
    }

    return hash;
}

}  // namespace TLSS::FileStorage::Transfer
namespace TLSS::FileStorage {
VerifyEngine::VerifyEngine(std::uint64_t size)
    : remaining_(size), digest_(Transfer::make_digest()) {}
void VerifyEngine::append(const void* data, std::size_t size) {
    if (size > remaining_) throw std::runtime_error("received more bytes than declared");
    if (size == 0) return;
    Transfer::update_digest(digest_.get(), data, size);
    remaining_ -= size;
}
const std::string& VerifyEngine::finish() {
    if (remaining_ != 0) throw std::runtime_error("file is incomplete");
    if (hash_.empty()) hash_ = Transfer::finish_digest(digest_.get());
    return hash_;
}
void VerifyEngine::verify(const std::string& expected_hash) {
    if (!expected_hash.empty() && finish() != expected_hash) {
        Transfer::transfer_logger()->error("SHA-256 mismatch");
        throw std::runtime_error("SHA-256 mismatch");
    }
}
}  // namespace TLSS::FileStorage
