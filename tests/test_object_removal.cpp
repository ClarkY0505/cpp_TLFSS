#include "ftp/object_storage_engine.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using TLSS::FileStorage::ObjectStorageEngine;

const std::string object_hash(64, 'a');

// 通过链接器包装系统调用，注入可重复的错误。
// 正常路径仍调用真实 unlink 和 fsync。
struct Injection {
    int unlink_calls = 0;
    int sync_calls = 0;
    int unlink_interrupts = 0;
    int sync_interrupts = 0;
    int unlink_error = 0;
    int sync_error = 0;

    fs::path root;
    fs::path object;
    bool sync_target_correct = true;
    bool sync_order_correct = true;
} injection;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 测试结束自动清理临时文件，不访问真实对象目录。
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/tlss-object-removal-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (!created) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(), "mkdtemp");
        }
        path = created;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    fs::path path;
};

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    output.close();
    check(output.good(), "create test file failed");
}

template <typename Operation>
void rejects_with_errno(Operation operation, int expected) {
    try {
        operation();
    } catch (const std::system_error& error) {
        check(error.code().value() == expected, "unexpected system error");
        return;
    }
    throw std::runtime_error("expected system error was not thrown");
}

}  // namespace

// GNU 链接器 --wrap 会将生产代码中的调用转到 __wrap_*。
// __real_* 用来调用原始系统函数。
extern "C" int __real_unlink(const char* path);
extern "C" int __real_fsync(int fd);

extern "C" int __wrap_unlink(const char* path) {
    ++injection.unlink_calls;

    if (injection.unlink_interrupts > 0) {
        --injection.unlink_interrupts;
        errno = EINTR;
        return -1;
    }

    if (injection.unlink_error != 0) {
        errno = injection.unlink_error;
        return -1;
    }

    return __real_unlink(path);
}

extern "C" int __wrap_fsync(int fd) {
    ++injection.sync_calls;

    // 确认同步的是对象根目录，而不是其他文件。
    struct stat actual {};
    struct stat expected {};
    if (::fstat(fd, &actual) != 0 ||
        ::stat(injection.root.c_str(), &expected) != 0 ||
        !S_ISDIR(actual.st_mode) ||
        actual.st_dev != expected.st_dev ||
        actual.st_ino != expected.st_ino) {
        injection.sync_target_correct = false;
    }

    // 同步必须发生在 unlink 之后，且此时对象目录项已经不存在。
    struct stat object_info {};
    const int object_result = ::lstat(injection.object.c_str(), &object_info);
    if (injection.unlink_calls == 0 || object_result != -1 || errno != ENOENT) {
        injection.sync_order_correct = false;
    }

    if (injection.sync_interrupts > 0) {
        --injection.sync_interrupts;
        errno = EINTR;
        return -1;
    }

    if (injection.sync_error != 0) {
        errno = injection.sync_error;
        return -1;
    }

    return __real_fsync(fd);
}

namespace {

void test_normal(ObjectStorageEngine& objects) {
    write_file(injection.object, "content");

    // 已发布对象是只读文件；删除依赖父目录权限，仍应成功。
    fs::permissions(injection.object,
                    fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);

    objects.remove_object(object_hash);

    check(!fs::exists(injection.object), "object was not removed");
    check(injection.unlink_calls == 1 && injection.sync_calls == 1,
          "unexpected removal operations");
}

void test_missing(ObjectStorageEngine& objects) {
    // 两次调用都遇到 ENOENT，也都必须同步目录。
    objects.remove_object(object_hash);
    objects.remove_object(object_hash);

    check(injection.unlink_calls == 2 && injection.sync_calls == 2,
          "missing object skipped directory synchronization");
}

void test_invalid_hash(ObjectStorageEngine& objects) {
    for (const auto& hash : {std::string{}, std::string("../escape"), std::string(63, 'a'),
                             std::string(65, 'a'), std::string(64, 'A')}) {
        bool rejected = false;
        try {
            objects.remove_object(hash);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid hash was accepted");
    }

    check(injection.unlink_calls == 0 && injection.sync_calls == 0,
          "invalid hash reached unlink or fsync");
}

void test_directory(ObjectStorageEngine& objects) {
    fs::create_directory(injection.object);

    rejects_with_errno([&] { objects.remove_object(object_hash); }, EISDIR);

    check(fs::is_directory(injection.object), "object directory was removed");
    check(injection.sync_calls == 0, "failed unlink continued to synchronization");
}

void test_symlink(ObjectStorageEngine& objects, const fs::path& temporary) {
    const auto target = temporary / "target";
    write_file(target, "unchanged");
    fs::create_symlink(target, injection.object);

    objects.remove_object(object_hash);

    check(!fs::is_symlink(injection.object), "object symlink was not removed");

    std::ifstream input(target);
    std::string content;
    input >> content;
    check(content == "unchanged", "symlink target was changed");
    check(injection.sync_calls == 1, "symlink removal did not synchronize directory");
}

void test_unlink_retry(ObjectStorageEngine& objects) {
    write_file(injection.object, "content");
    injection.unlink_interrupts = 1;

    objects.remove_object(object_hash);

    check(!fs::exists(injection.object), "unlink retry failed");
    check(injection.unlink_calls == 2 && injection.sync_calls == 1,
          "unlink EINTR was not retried correctly");
}

void test_unlink_error(ObjectStorageEngine& objects) {
    write_file(injection.object, "content");
    injection.unlink_error = EACCES;

    rejects_with_errno([&] { objects.remove_object(object_hash); }, EACCES);

    check(fs::exists(injection.object), "object changed after failed unlink");
    check(injection.unlink_calls == 1 && injection.sync_calls == 0,
          "unlink failure was retried or ignored");
}

void test_sync_retry(ObjectStorageEngine& objects) {
    write_file(injection.object, "content");
    injection.sync_interrupts = 1;

    objects.remove_object(object_hash);

    check(!fs::exists(injection.object), "object was not removed");
    check(injection.unlink_calls == 1 && injection.sync_calls == 2,
          "fsync EINTR was not retried correctly");
}

void test_sync_error(ObjectStorageEngine& objects) {
    write_file(injection.object, "content");
    injection.sync_error = EIO;

    rejects_with_errno([&] { objects.remove_object(object_hash); }, EIO);

    // unlink 已成功，但同步失败，必须向上传播错误。
    check(!fs::exists(injection.object), "unlink did not finish before synchronization");
    check(injection.unlink_calls == 1 && injection.sync_calls == 1,
          "unexpected operations after synchronization failure");

    // 模拟下一次恢复：对象已经不存在，仍须重新同步目录。
    injection.sync_error = 0;
    objects.remove_object(object_hash);

    check(injection.unlink_calls == 2 && injection.sync_calls == 2,
          "recovery skipped directory synchronization");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);

        TemporaryDirectory temporary;
        injection.root = temporary.path / "objects";
        injection.object = injection.root / object_hash;
        fs::create_directory(injection.root);

        check(::setenv("TLSS_STORAGE_ROOT", injection.root.c_str(), 1) == 0,
              "set storage root failed");

        ObjectStorageEngine objects;

        if (scenario == "normal")
            test_normal(objects);
        else if (scenario == "missing")
            test_missing(objects);
        else if (scenario == "invalid_hash")
            test_invalid_hash(objects);
        else if (scenario == "directory")
            test_directory(objects);
        else if (scenario == "symlink")
            test_symlink(objects, temporary.path);
        else if (scenario == "unlink_retry")
            test_unlink_retry(objects);
        else if (scenario == "unlink_error")
            test_unlink_error(objects);
        else if (scenario == "sync_retry")
            test_sync_retry(objects);
        else if (scenario == "sync_error")
            test_sync_error(objects);
        else
            throw std::invalid_argument("unknown scenario: " + scenario);

        check(injection.sync_target_correct, "fsync targeted the wrong file or directory");
        check(injection.sync_order_correct, "directory synchronization happened before removal");

        std::cout << "PASS object removal " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
