#include "auth/password_hasher.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace TLSS::AUTH {
namespace {

constexpr std::size_t k_salt_bytes = 16;
constexpr std::size_t k_key_bytes = 32;
constexpr int k_iterations = 600000;
constexpr int k_max_iterations = 2000000;

constexpr std::string_view k_hash_prefix = "pbkdf2-sha256-v1$";

std::string encode_hex(const unsigned char* data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";

  std::string result(size * 2, '\0');

  for (std::size_t i = 0; i < size; ++i) {
    result[i * 2] = digits[data[i] >> 4];
    result[i * 2 + 1] = digits[data[i] & 0x0f];
  }

  return result;
}

int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }

  return -1;
}

template <std::size_t Size>
std::array<unsigned char, Size> decode_hex(std::string_view text) {
  if (text.size() != Size * 2) {
    throw std::invalid_argument("Invalid credential encoding length");
  }

  std::array<unsigned char, Size> result{};

  for (std::size_t i = 0; i < Size; ++i) {
    const int high = hex_value(text[i * 2]);
    const int low = hex_value(text[i * 2 + 1]);

    if (high < 0 || low < 0) {
      throw std::invalid_argument("Invalid credential encoding");
    }

    result[i] = static_cast<unsigned char>((high << 4) | low);
  }

  return result;
}

std::array<unsigned char, k_key_bytes> derive_key(
    std::string_view password, const std::array<unsigned char, k_salt_bytes>& salt,
    int iterations) {
  std::array<unsigned char, k_key_bytes> result{};

  const int status =
      PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                        static_cast<int>(salt.size()), iterations, EVP_sha256(),
                        static_cast<int>(result.size()), result.data());

  if (status != 1) {
    throw std::runtime_error("Password derivation failed");
  }

  return result;
}

}  // namespace

PasswordCredentials PasswordHasher::hash_password(std::string_view password) {
  if (password.size() < 8 || password.size() > 128) {
    throw std::invalid_argument("Password must contain 8 to 128 bytes");
  }

  std::array<unsigned char, k_salt_bytes> salt{};

  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
    throw std::runtime_error("Salt generation failed");
  }

  auto key = derive_key(password, salt, k_iterations);

  PasswordCredentials credentials;
  credentials.salt = encode_hex(salt.data(), salt.size());
  credentials.password_hash = std::string(k_hash_prefix) + std::to_string(k_iterations) + "$" +
                              encode_hex(key.data(), key.size());

  OPENSSL_cleanse(key.data(), key.size());

  return credentials;
}

bool PasswordHasher::verify_password(std::string_view password,
                                     const PasswordCredentials& credentials) {
  if (password.empty() || password.size() > 128) {
    return false;
  }

  const std::string_view encoded = credentials.password_hash;

  if (encoded.size() > 255 || encoded.substr(0, k_hash_prefix.size()) != k_hash_prefix) {
    throw std::invalid_argument("Unsupported password format");
  }

  const auto payload = encoded.substr(k_hash_prefix.size());
  const auto separator = payload.find('$');

  if (separator == std::string_view::npos) {
    throw std::invalid_argument("Invalid password format");
  }

  const auto iterations_text = payload.substr(0, separator);
  const auto hash_text = payload.substr(separator + 1);

  int iterations = 0;

  const auto parsed = std::from_chars(iterations_text.data(),
                                      iterations_text.data() + iterations_text.size(), iterations);

  if (parsed.ec != std::errc{} || parsed.ptr != iterations_text.data() + iterations_text.size() ||
      iterations < k_iterations || iterations > k_max_iterations) {
    throw std::invalid_argument("Invalid password iteration count");
  }

  const auto salt = decode_hex<k_salt_bytes>(credentials.salt);
  const auto expected = decode_hex<k_key_bytes>(hash_text);
  auto actual = derive_key(password, salt, iterations);

  const bool matched = CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;

  OPENSSL_cleanse(actual.data(), actual.size());

  return matched;
}

}  // namespace TLSS::AUTH
