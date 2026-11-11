#include "ftp/storage_root_lock.h"

#include <sys/stat.h>
#include <sys/wait.h>
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
using TLSS::FileStorage::StorageRootLock;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 每个测试进程使用独立临时目录，不访问真实对象目录或数据库。
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/tlss-root-lock-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (!created) {
            const int error_code = errno;
            throw std::system_error(error_code, std::generic_category(), "mkdtemp");
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

template <typename Operation>
void rejects_with_errno(Operation operation, int expected, const char* message) {
    try {
        operation();
    } catch (const std::system_error& error) {
        check(error.code().value() == expected, "unexpected system error");
        return;
    }
    throw std::runtime_error(message);
}

void rejects_locked(const fs::path& root) {
    rejects_with_errno(
        [&] { StorageRootLock duplicate(root); },
        EWOULDBLOCK,
        "locked directory accepted another owner");
}

// fork 前测试程序没有创建线程；子进程通过退出码向父进程报告结果。
// _exit 避免子进程执行继承来的临时目录析构函数。
template <typename Operation>
void run_child(Operation operation) {
    const pid_t child = ::fork();
    if (child == -1) {
        const int error_code = errno;
        throw std::system_error(error_code, std::generic_category(), "fork");
    }
    if (child == 0) {
        try {
            operation();
            ::_exit(0);
        } catch (const std::exception& error) {
            std::cerr << "child: " << error.what() << '\n';
            ::_exit(1);
        } catch (...) {
            ::_exit(1);
        }
    }

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        const int error_code = errno;
        throw std::system_error(error_code, std::generic_category(), "waitpid");
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child process failed");
}

void test_exclusive(const fs::path& base) {
    const auto root = base / "nested" / "objects";
    const auto lock_path = root / ".tlss.lock";
    struct stat original {};
    {
        StorageRootLock first(root);
        check(fs::is_regular_file(lock_path), "lock file was not created");
        check(::stat(lock_path.c_str(), &original) == 0, "stat lock file failed");
        rejects_locked(root);
        // 加锁失败的临时实例销毁后，也不能释放原实例的锁。
        rejects_locked(root);
    }

    // 析构释放锁，但保留同一个锁文件，供后续实例继续使用。
    check(fs::is_regular_file(lock_path), "lock file was removed on destruction");
    StorageRootLock next(root);
    struct stat current {};
    check(::stat(lock_path.c_str(), &current) == 0, "stat reused lock file failed");
    check(original.st_dev == current.st_dev && original.st_ino == current.st_ino,
          "lock file was replaced instead of reused");
    rejects_locked(root);
}

void test_independent(const fs::path& base) {
    StorageRootLock first(base / "first");
    StorageRootLock second(base / "second");
    rejects_locked(base / "first");
    rejects_locked(base / "second");
}

void test_exception(const fs::path& root) {
    // 模拟启动准备抛异常，验证栈展开能释放目录锁。
    bool acquired = false;
    bool caught = false;
    try {
        StorageRootLock lock(root);
        acquired = true;
        throw std::runtime_error("startup failed");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(acquired && caught, "startup exception did not follow lock acquisition");
    StorageRootLock recovered(root);
    rejects_locked(root);
}

void test_invalid_root(const fs::path& base) {
    const auto blocked = base / "file";
    {
        std::ofstream output(blocked);
        output << "not a directory";
        check(output.good(), "create blocking file failed");
    }
    rejects_with_errno(
        [&] { StorageRootLock lock(blocked / "objects"); },
        ENOTDIR,
        "regular file accepted as parent directory");
}

void test_symlink(const fs::path& base) {
    const auto root = base / "objects";
    const auto target = base / "target";
    fs::create_directories(root);
    {
        std::ofstream output(target);
        output << "unchanged";
        check(output.good(), "create symlink target failed");
    }
    fs::create_symlink(target, root / ".tlss.lock");
    rejects_with_errno(
        [&] { StorageRootLock lock(root); },
        ELOOP,
        "symbolic link accepted as lock file");
    check(fs::is_symlink(root / ".tlss.lock"), "lock symlink was changed");
    std::ifstream input(target);
    std::string content;
    input >> content;
    check(content == "unchanged", "symlink target was changed");
}

void test_invalid_lock(const fs::path& root) {
    const auto lock_path = root / ".tlss.lock";
    fs::create_directories(lock_path);
    rejects_with_errno(
        [&] { StorageRootLock lock(root); },
        EISDIR,
        "directory accepted as lock file");
    fs::remove(lock_path);

    // Linux 上 O_RDWR 打开 FIFO 不阻塞，随后应被普通文件检查拒绝。
    check(::mkfifo(lock_path.c_str(), 0600) == 0, "create FIFO failed");
    bool rejected = false;
    try {
        StorageRootLock lock(root);
    } catch (const std::system_error&) {
        throw;  // 这里应当在 fstat 成功后拒绝非普通文件。
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "FIFO accepted as lock file");
    fs::remove(lock_path);
    StorageRootLock recovered(root);
    rejects_locked(root);
}

void test_process_exclusive(const fs::path& root) {
    StorageRootLock parent_lock(root);
    run_child([&] {
        // 子进程重新 open 同一锁文件，必须被父进程持有的锁阻止。
        rejects_locked(root);
    });
    // 子进程退出不能释放父进程仍持有的锁。
    rejects_locked(root);
}

void test_process_exit(const fs::path& root) {
    run_child([&] {
        StorageRootLock child_lock(root);
        // 在锁对象仍存活时直接退出，不执行 C++ 析构。
        // 用于验证进程退出后内核关闭句柄并释放锁的行为。
        ::_exit(0);
    });
    StorageRootLock recovered(root);
    rejects_locked(root);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        TemporaryDirectory temporary;
        if (scenario == "exclusive")
            test_exclusive(temporary.path);
        else if (scenario == "independent")
            test_independent(temporary.path);
        else if (scenario == "exception")
            test_exception(temporary.path);
        else if (scenario == "invalid_root")
            test_invalid_root(temporary.path);
        else if (scenario == "symlink")
            test_symlink(temporary.path);
        else if (scenario == "invalid_lock")
            test_invalid_lock(temporary.path);
        else if (scenario == "process_exclusive")
            test_process_exclusive(temporary.path);
        else if (scenario == "process_exit")
            test_process_exit(temporary.path);
        else
            throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS storage root lock " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
