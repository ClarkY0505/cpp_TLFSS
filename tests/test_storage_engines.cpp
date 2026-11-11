#include <unistd.h>
#include <system_error>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ftp/object_storage_engine.h"
#include "ftp/verify_engine.h"

namespace {
void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Operation>
void rejects(Operation operation, const char* message) {
  try {
    operation();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace TLSS::FileStorage;
  const std::string abc_hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  char pattern[] = "/tmp/tlss-object-engines-XXXXXX";
  const char* created = ::mkdtemp(pattern);
  if (!created)
    return 1;
  const std::filesystem::path directory(created);
  try {
    ::setenv("TLSS_STORAGE_ROOT", directory.c_str(), 1);
    VerifyEngine verifier(3);
    verifier.append("a", 1);
    rejects(
        [&] {
          verifier.finish();
        },
        "incomplete content accepted");
    rejects(
        [&] {
          verifier.append("bcd", 3);
        },
        "oversized content accepted");
    verifier.append("bc", 2);
    check(verifier.finish() == abc_hash, "streaming digest differs");
    check(verifier.finish() == abc_hash, "digest finalization is not repeatable");
    rejects(
        [&] {
          verifier.verify(std::string(64, '0'));
        },
        "wrong digest accepted");
    verifier.verify(abc_hash);
    VerifyEngine empty(0);
    check(empty.finish() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty digest differs");

    ObjectStorageEngine objects;
    auto writer = objects.create_temporary(3);
    writer->append("abc", 3);
    const auto published = objects.publish(*writer);
    check(published.hash == abc_hash && published.size == 3, "object identity differs");
    check(objects.verify_object(abc_hash), "valid object verification failed");
    auto reader = objects.open_read(abc_hash);
    check(reader->size == 3, "object length differs");
    check(ObjectStorageEngine::read(reader, 1, 2) == "bc", "offset read differs");
    rejects(
        [&] {
          objects.open_read("../escape");
        },
        "invalid object hash accepted");
    auto duplicate = objects.create_temporary(3);
    duplicate->append("abc", 3);
    check(objects.publish(*duplicate).hash == abc_hash, "same-content reuse failed");

    // An existing content-addressed object must be validated before reuse.
    std::filesystem::remove(directory / abc_hash);
    std::ofstream(directory / abc_hash) << "bad";
    check(!objects.verify_object(abc_hash), "corrupt object accepted");
    auto inconsistent = objects.create_temporary(3);
    inconsistent->append("abc", 3);
    rejects(
        [&] {
          objects.publish(*inconsistent);
        },
        "inconsistent object reused");
    inconsistent.reset();
    auto cancelled = objects.create_temporary(4);
    cancelled->append("ab", 2);
    cancelled.reset();
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      check(entry.path().filename().string().find(".tlss-part-") != 0, "temporary object leaked");
    }
    bool missing = false;
    try {
      objects.verify_object(std::string(64, '0'));
    } catch (const std::system_error& error) {
      missing = error.code() == std::errc::no_such_file_or_directory;
    }
    check(missing, "missing object did not preserve ENOENT");
    std::filesystem::remove_all(directory);
    std::cout << "PASS storage engines\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
