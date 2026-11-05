#ifndef TLSS_FTP_VERIFY_ENGINE_H
#define TLSS_FTP_VERIFY_ENGINE_H

#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "observability/storage_metrics.h"

namespace TLSS::FileStorage::Transfer {
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
Digest make_digest();
void update_digest(EVP_MD_CTX* digest, const void* data, std::size_t size);
std::string finish_digest(EVP_MD_CTX* digest);
}  // namespace TLSS::FileStorage::Transfer
namespace TLSS::FileStorage {
// File integrity only; independent of user authentication and execution resources.
class VerifyEngine {
 public:
  explicit VerifyEngine(std::uint64_t size);
  void set_metrics(std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics) {
    metrics_ = std::move(metrics);
  }
  void append(const void* data, std::size_t size);
  const std::string& finish();
  void verify(const std::string& expected_hash);

 private:
  std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics_;
  std::uint64_t remaining_;
  Transfer::Digest digest_;
  std::string hash_;
};
}  // namespace TLSS::FileStorage
#endif
