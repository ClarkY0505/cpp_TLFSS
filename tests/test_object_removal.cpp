#include "ftp/object_storage_engine.h"
#include "ftp/storage_root_lock.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

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
    int unlink_error_after = 0;
    int unlink_missing = 0;
    int sync_error = 0;
    bool cleanup_mode = false;
    std::vector<fs::path> absent_before_sync;

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

    if (injection.unlink_error != 0 && injection.unlink_calls > injection.unlink_error_after) {
        errno = injection.unlink_error;
        return -1;
    }

    if (injection.unlink_missing > 0) {
        --injection.unlink_missing;
        // 模拟检查类型后、实际 unlink 前，文件已被其他操作移除。
        if (__real_unlink(path) == -1)
            return -1;
        errno = ENOENT;
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
    if (injection.cleanup_mode) {
        // 临时文件清理允许零次 unlink；正式哈希文件可以仍然存在。
        for (const auto& path : injection.absent_before_sync) {
            struct stat info {};
            if (::lstat(path.c_str(), &info) != -1 || errno != ENOENT)
                injection.sync_order_correct = false;
        }
    } else {
        struct stat object_info {};
        const int object_result = ::lstat(injection.object.c_str(), &object_info);
        if (injection.unlink_calls == 0 || object_result != -1 || errno != ENOENT)
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

fs::path cleanup_file(const std::string& suffix) {
    const auto path = injection.root / (".tlss-part-" + suffix);
    write_file(path, "partial content");
    injection.absent_before_sync.push_back(path);
    return path;
}

void test_cleanup_empty(ObjectStorageEngine& objects) {
    check(objects.cleanup_temporary_files() == 0, "empty cleanup returned a nonzero count");
    check(injection.unlink_calls == 0 && injection.sync_calls == 1,
          "empty cleanup skipped directory synchronization");
}

void test_cleanup_matching(ObjectStorageEngine& objects) {
    cleanup_file("abc123");
    cleanup_file("ABC123");
    const auto readonly = cleanup_file("aB09zZ");
    fs::permissions(readonly, fs::perms::owner_read);
    check(objects.cleanup_temporary_files() == 3, "matching temporary files were not counted");
    check(injection.unlink_calls == 3 && injection.sync_calls == 1,
          "cleanup did not remove each candidate and synchronize once");
    check(objects.cleanup_temporary_files() == 0, "repeated cleanup was not empty");
    check(injection.unlink_calls == 3 && injection.sync_calls == 2,
          "repeated cleanup skipped synchronization or deleted extra files");
}

void test_cleanup_ignored(ObjectStorageEngine& objects) {
    const std::vector<std::string> names{
        object_hash, "unrelated", ".tlss-part-", ".tlss-part-ABCDE", ".tlss-part-ABCDEFG",
        ".tlss-part-abc_12", ".tlss-part-abc-12", "other-tlss-part-ABC123",
    };
    for (const auto& name : names)
        write_file(injection.root / name, "preserved");
    check(objects.cleanup_temporary_files() == 0, "cleanup accepted an unrelated filename");
    check(injection.unlink_calls == 0 && injection.sync_calls == 1,
          "ignored files reached unlink or skipped synchronization");
    for (const auto& name : names)
        check(fs::exists(injection.root / name), "cleanup removed an unrelated file");
}

void test_cleanup_types(ObjectStorageEngine& objects, const fs::path& temporary) {
    const auto directory = injection.root / ".tlss-part-dir123";
    fs::create_directory(directory);
    const auto nested = directory / ".tlss-part-ABC123";
    write_file(nested, "nested");

    const auto target = temporary / "outside";
    write_file(target, "outside");
    const auto link = injection.root / ".tlss-part-link12";
    fs::create_symlink(target, link);
    const auto dangling = injection.root / ".tlss-part-dead12";
    fs::create_symlink(temporary / "absent", dangling);
    const auto fifo = injection.root / ".tlss-part-pipe12";
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create cleanup FIFO failed");

    const auto outside_directory = temporary / "outside-directory";
    fs::create_directory(outside_directory);
    const auto outside_child = outside_directory / ".tlss-part-DEF456";
    write_file(outside_child, "outside directory");
    const auto directory_link = injection.root / ".tlss-part-dirlnk";
    fs::create_directory_symlink(outside_directory, directory_link);

    check(objects.cleanup_temporary_files() == 0, "cleanup accepted a non-regular entry");
    check(injection.unlink_calls == 0 && injection.sync_calls == 1,
          "cleanup attempted to remove a non-regular entry");
    check(fs::is_directory(directory) && fs::exists(nested) && fs::is_symlink(link) &&
              fs::exists(target) && fs::is_symlink(dangling) && fs::is_fifo(fifo) &&
              fs::is_symlink(directory_link) && fs::exists(outside_child),
          "cleanup followed a symlink, recursed, or removed a special entry");
}

void test_cleanup_hard_link(ObjectStorageEngine& objects) {
    // 发布后崩溃可能留下与正式对象指向同一 inode 的临时名字。
    // 删除临时名字必须保留正式哈希名字和内容。
    write_file(injection.object, "published");
    const auto temporary = injection.root / ".tlss-part-Ab12c3";
    fs::create_hard_link(injection.object, temporary);
    injection.absent_before_sync.push_back(temporary);
    check(objects.cleanup_temporary_files() == 1, "hard-linked temporary file was not removed");
    check(fs::exists(injection.object) && !fs::exists(temporary),
          "cleanup removed the published object instead of its temporary name");
    std::ifstream input(injection.object);
    std::string content;
    input >> content;
    check(content == "published", "published object content changed during cleanup");
}

void test_cleanup_unlink_retry(ObjectStorageEngine& objects) {
    cleanup_file("Ab12c3");
    injection.unlink_interrupts = 1;
    check(objects.cleanup_temporary_files() == 1, "cleanup unlink retry changed the count");
    check(injection.unlink_calls == 2 && injection.sync_calls == 1,
          "cleanup did not retry unlink after EINTR");
}

void test_cleanup_missing(ObjectStorageEngine& objects) {
    const auto path = cleanup_file("Ab12c3");
    injection.unlink_missing = 1;
    check(objects.cleanup_temporary_files() == 0, "ENOENT was counted as a successful unlink");
    check(!fs::exists(path) && injection.unlink_calls == 1 && injection.sync_calls == 1,
          "cleanup did not tolerate ENOENT and synchronize the directory");
}

void test_cleanup_unlink_error(ObjectStorageEngine& objects) {
    const auto path = cleanup_file("Ab12c3");
    injection.unlink_error = EACCES;
    rejects_with_errno([&] { objects.cleanup_temporary_files(); }, EACCES);
    check(fs::exists(path) && injection.unlink_calls == 1 && injection.sync_calls == 0,
          "cleanup ignored an unlink error or continued after it");
}

void test_cleanup_partial_failure(ObjectStorageEngine& objects) {
    const auto first = cleanup_file("Ab12c3");
    const auto second = cleanup_file("De45f6");
    // 不依赖目录遍历顺序：第一次删除成功，第二次删除注入错误。
    injection.unlink_error = EIO;
    injection.unlink_error_after = 1;
    rejects_with_errno([&] { objects.cleanup_temporary_files(); }, EIO);
    check(static_cast<int>(fs::exists(first)) + static_cast<int>(fs::exists(second)) == 1,
          "partial cleanup did not retain exactly one pending file");
    check(injection.unlink_calls == 2 && injection.sync_calls == 0,
          "partial cleanup did not stop at the failed unlink");
    injection.unlink_error = 0;
    check(objects.cleanup_temporary_files() == 1, "cleanup retry did not finish the remaining file");
    check(!fs::exists(first) && !fs::exists(second) && injection.sync_calls == 1,
          "cleanup retry lost prior progress or skipped synchronization");
}

void test_cleanup_sync_retry(ObjectStorageEngine& objects) {
    cleanup_file("Ab12c3");
    injection.sync_interrupts = 1;
    check(objects.cleanup_temporary_files() == 1, "cleanup fsync retry changed the count");
    check(injection.unlink_calls == 1 && injection.sync_calls == 2,
          "cleanup did not retry directory fsync after EINTR");
}

void test_cleanup_sync_error(ObjectStorageEngine& objects) {
    const auto path = cleanup_file("Ab12c3");
    injection.sync_error = EIO;
    rejects_with_errno([&] { objects.cleanup_temporary_files(); }, EIO);
    check(!fs::exists(path), "cleanup fsync happened before unlink");
    injection.sync_error = 0;
    check(objects.cleanup_temporary_files() == 0, "retry counted an already removed temporary file");
    check(injection.unlink_calls == 1 && injection.sync_calls == 2,
          "empty retry skipped the previously failed directory synchronization");
}

void test_cleanup_crash(ObjectStorageEngine& objects) {
    // 使用真实 Receiver 创建未完成上传，直接退出子进程以跳过临时文件析构。
    // 父进程等待子进程结束后再清理，此时没有活动传输。
    const pid_t child = ::fork();
    check(child != -1, "fork failed for interrupted upload");
    if (child == 0) {
        try {
            auto writer = objects.create_temporary(8);
            writer->append("abc", 3);
            ::_exit(0);
        } catch (...) {
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "interrupted upload child failed");
    for (const auto& entry : fs::directory_iterator(injection.root)) {
        if (entry.path().filename().string().find(".tlss-part-") == 0)
            injection.absent_before_sync.push_back(entry.path());
    }
    check(injection.absent_before_sync.size() == 1, "interrupted upload left no temporary file");
    check(objects.cleanup_temporary_files() == 1, "actual Receiver temporary file was not cleaned");
    check(injection.unlink_calls == 1 && injection.sync_calls == 1,
          "interrupted upload cleanup skipped deletion or synchronization");
}

void run_cleanup_scenario(const std::string& scenario, ObjectStorageEngine& objects,
                          const fs::path& temporary) {
    if (scenario == "cleanup_empty")
        test_cleanup_empty(objects);
    else if (scenario == "cleanup_matching")
        test_cleanup_matching(objects);
    else if (scenario == "cleanup_ignored")
        test_cleanup_ignored(objects);
    else if (scenario == "cleanup_types")
        test_cleanup_types(objects, temporary);
    else if (scenario == "cleanup_hard_link")
        test_cleanup_hard_link(objects);
    else if (scenario == "cleanup_unlink_retry")
        test_cleanup_unlink_retry(objects);
    else if (scenario == "cleanup_missing")
        test_cleanup_missing(objects);
    else if (scenario == "cleanup_unlink_error")
        test_cleanup_unlink_error(objects);
    else if (scenario == "cleanup_partial_failure")
        test_cleanup_partial_failure(objects);
    else if (scenario == "cleanup_sync_retry")
        test_cleanup_sync_retry(objects);
    else if (scenario == "cleanup_sync_error")
        test_cleanup_sync_error(objects);
    else if (scenario == "cleanup_crash")
        test_cleanup_crash(objects);
    else
        throw std::invalid_argument("unknown cleanup scenario: " + scenario);
}

void test_scan_empty(ObjectStorageEngine& objects) {
    check(objects.list_object_hashes().empty(), "empty directory produced object candidates");
}

void test_scan_sorted(ObjectStorageEngine& objects) {
    const std::string low(64, '0');
    const std::string high(64, 'f');
    // 故意按不同于字典序的顺序创建；内容不匹配名字也应列为候选。
    write_file(injection.root / high, "unverified content");
    write_file(injection.root / object_hash, "");
    write_file(injection.root / low, "corrupt content");
    check(objects.list_object_hashes() == std::vector<std::string>{low, object_hash, high},
          "object candidates were unsorted, content-validated, or incomplete");
}

void test_scan_ignored(ObjectStorageEngine& objects) {
    const std::vector<std::string> names{
        ".tlss-part-Ab12c3", "unrelated", std::string(63, 'a'), std::string(65, 'a'),
        std::string(64, 'A'), std::string(64, 'g'),
    };
    for (const auto& name : names)
        write_file(injection.root / name, "preserved");
    check(objects.list_object_hashes().empty(), "scan accepted a non-hash filename");
    for (const auto& name : names)
        check(fs::exists(injection.root / name), "scan modified an ignored entry");
}

void test_scan_types(ObjectStorageEngine& objects, const fs::path& temporary) {
    const auto directory = injection.root / std::string(64, '1');
    fs::create_directory(directory);
    const auto nested = directory / std::string(64, '0');
    write_file(nested, "nested");
    const auto outside = temporary / "outside";
    write_file(outside, "outside");
    const auto link = injection.root / std::string(64, '2');
    fs::create_symlink(outside, link);
    const auto dangling = injection.root / std::string(64, '3');
    fs::create_symlink(temporary / "absent", dangling);
    const auto fifo = injection.root / std::string(64, '4');
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create scan FIFO failed");
    const auto directory_link = injection.root / std::string(64, '5');
    fs::create_directory_symlink(directory, directory_link);
    const std::string regular_hash(64, '6');
    write_file(injection.root / regular_hash, "regular");

    check(objects.list_object_hashes() == std::vector<std::string>{regular_hash},
          "scan followed a symlink, recursed, or accepted a special entry");
    check(fs::exists(nested) && fs::is_symlink(link) && fs::is_symlink(dangling) &&
              fs::is_fifo(fifo) && fs::is_symlink(directory_link) && fs::exists(outside),
          "scan changed a directory, symlink, or special entry");
}

void test_scan_failure(ObjectStorageEngine& objects, const fs::path& temporary) {
    // 模拟根目录损坏：保留原目录及其锁 inode，在原路径放一个普通文件。
    const auto preserved = temporary / "preserved-root";
    fs::rename(injection.root, preserved);
    write_file(injection.root, "not a directory");
    bool caught = false;
    try {
        objects.list_object_hashes();
    } catch (const fs::filesystem_error&) {
        caught = true;
    }
    fs::remove(injection.root);
    fs::rename(preserved, injection.root);
    check(caught, "scan converted a filesystem error to an empty candidate list");
}

void run_scan_scenario(const std::string& scenario, ObjectStorageEngine& objects,
                       const fs::path& temporary) {
    if (scenario == "scan_empty")
        test_scan_empty(objects);
    else if (scenario == "scan_sorted")
        test_scan_sorted(objects);
    else if (scenario == "scan_ignored")
        test_scan_ignored(objects);
    else if (scenario == "scan_types")
        test_scan_types(objects, temporary);
    else if (scenario == "scan_failure")
        test_scan_failure(objects, temporary);
    else
        throw std::invalid_argument("unknown scan scenario: " + scenario);
    check(injection.unlink_calls == 0 && injection.sync_calls == 0,
          "read-only object scan called unlink or fsync");
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

        injection.cleanup_mode = scenario.find("cleanup_") == 0;
        const bool scan_mode = scenario.find("scan_") == 0;
        std::unique_ptr<TLSS::FileStorage::StorageRootLock> root_lock;
        if (injection.cleanup_mode || scan_mode)
            root_lock = std::make_unique<TLSS::FileStorage::StorageRootLock>(injection.root);
        ObjectStorageEngine objects;

        if (scan_mode)
            run_scan_scenario(scenario, objects, temporary.path);
        else if (injection.cleanup_mode)
            run_cleanup_scenario(scenario, objects, temporary.path);
        else if (scenario == "normal")
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
        if (injection.cleanup_mode || scan_mode)
            check(fs::is_regular_file(injection.root / ".tlss.lock"), "cleanup removed the lock file");

        std::cout << "PASS object removal " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
