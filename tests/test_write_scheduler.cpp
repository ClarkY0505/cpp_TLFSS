#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/net/event_loop.h"
#include "common/net/write_scheduler.h"

namespace {
using namespace TLSS::NET;

void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct Fixture {
  EventLoop loop;
  int sockets[2]{-1, -1};
  bool writable = false;
  int drained = 0;
  int failed = 0;
  std::size_t high_water = 0;
  std::unique_ptr<WriteScheduler> scheduler;

  Fixture() {
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
          "socketpair failed");
    int small = 4096;
    check(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0,
          "setsockopt failed");
    WriteScheduler::Hooks hooks;
    hooks.write_interest = [this](bool enabled) {
      writable = enabled;
    };
    hooks.high_water = [this](std::size_t size) {
      high_water = size;
    };
    hooks.drained = [this] {
      ++drained;
    };
    hooks.failed = [this](int error) {
      failed = error;
    };
    hooks.make_read_completion = [this](std::uint64_t sequence) {
      return [this, sequence](std::error_code error, std::string data) {
        loop.queue_in_loop([this, sequence, error, data = std::move(data)]() mutable {
          scheduler->on_file_read(sequence, error, std::move(data));
        });
      };
    };
    scheduler = std::make_unique<WriteScheduler>(&loop, sockets[0], std::move(hooks));
  }

  ~Fixture() {
    scheduler.reset();
    ::close(sockets[0]);
    ::close(sockets[1]);
  }

  void pump() {
    loop.queue_in_loop([this] {
      loop.quit();
    });
    loop.wakeup();
    loop.loop();
  }

  std::string receive() {
    std::string data;
    char block[8192];
    ssize_t count;
    while ((count = ::recv(sockets[1], block, sizeof block, 0)) > 0)
      data.append(block, static_cast<std::size_t>(count));
    check(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK, "recv failed");
    return data;
  }

  std::string finish() {
    std::string wire;
    for (unsigned step = 0; step < 2000; ++step) {
      if (writable)
        scheduler->on_writable();
      pump();
      wire += receive();
      if (scheduler->empty()) {
        pump();
        return wire;
      }
    }
    throw std::runtime_error("scheduler failed to drain");
  }
};

void mixed_fifo() {
  Fixture f;
  const std::string memory(180000, 'M');
  const std::string async(2 * 65536 + 37, 'A');
  const std::string file(100000, 'F');
  char path[] = "/tmp/tlss-scheduler-XXXXXX";
  const int fd = ::mkstemp(path);
  check(fd >= 0, "mkstemp failed");
  ::unlink(path);
  check(::write(fd, file.data(), file.size()) == static_cast<ssize_t>(file.size()),
        "file write failed");
  auto prepared = WriteScheduler::prepare_file(fd, 7, file.size() - 7, {});
  ::close(fd);
  int callbacks = 0;
  std::vector<off_t> offsets;
  auto reader = [&](off_t offset, std::size_t count, detail::FileReadCompletion completion) {
    offsets.push_back(offset);
    check(count <= 65536, "chunk exceeded 64 KiB");
    completion({}, async.substr(static_cast<std::size_t>(offset - 9), count));
  };
  f.scheduler->set_high_water_mark(200000);
  f.scheduler->enqueue_memory(memory);
  f.scheduler->enqueue_file(WriteScheduler::prepare_async(
      9, async.size(), reader, [&](std::error_code error, std::uint64_t sent) {
        check(!error && sent == async.size(), "incorrect async completion");
        ++callbacks;
      }));
  f.scheduler->enqueue_file(std::move(prepared));
  f.scheduler->enqueue_memory("TAIL");
  check(f.high_water == memory.size() + 65536, "incorrect reservation/high water");
  f.scheduler->on_writable();
  check(!f.scheduler->empty() && callbacks == 0, "partial write lost queued tasks");
  auto wire = f.receive();
  wire += f.finish();
  check(wire == memory + async + file.substr(7) + "TAIL", "mixed FIFO corrupted wire order");
  check(offsets == std::vector<off_t>({9, 65545, 131081}), "incorrect read offsets");
  check(callbacks == 1 && f.drained == 1 && f.failed == 0, "incorrect completion count");
  check(f.scheduler->pending_memory_bytes() == 0, "queue memory leaked");
}

void waiting_cancel_and_late_completion() {
  Fixture f;
  detail::FileReadCompletion read;
  int callbacks = 0;
  f.scheduler->enqueue_file(WriteScheduler::prepare_async(
      0, 3,
      [&](off_t, std::size_t, detail::FileReadCompletion completion) {
        read = std::move(completion);
      },
      [&](std::error_code error, std::uint64_t sent) {
        check(error == std::errc::operation_canceled && sent == 0, "incorrect cancellation");
        ++callbacks;
      }));
  f.scheduler->enqueue_memory("TAIL");
  f.scheduler->on_writable();
  check(read && !f.writable && !f.scheduler->empty(), "waiting read kept write interest");
  check(f.receive().empty(), "tail bypassed waiting file");
  f.scheduler->cancel_all(ECANCELED);
  read({}, "BAD");
  read({}, "BAD");
  f.pump();
  check(callbacks == 1 && f.scheduler->empty() && !f.writable, "late read revived scheduler");
  check(f.receive().empty() && f.drained == 0, "cancellation reported normal drain");
}

void reader_cancels_synchronously() {
  Fixture f;
  int callbacks = 0;
  f.scheduler->enqueue_file(WriteScheduler::prepare_async(
      0, 3,
      [&](off_t, std::size_t, detail::FileReadCompletion completion) {
        f.scheduler->cancel_all(ECANCELED);
        completion({}, "BAD");
      },
      [&](std::error_code error, std::uint64_t sent) {
        check(error == std::errc::operation_canceled && sent == 0,
              "incorrect reentrant cancellation");
        ++callbacks;
      }));
  f.scheduler->enqueue_memory("TAIL");
  f.scheduler->on_writable();
  f.pump();
  check(callbacks == 1 && f.scheduler->empty() && !f.writable, "reader cancellation revived queue");
  check(f.receive().empty() && f.drained == 0 && f.failed == 0,
        "reader cancellation sent data or reported drain");
}

void worker_completion_and_stale_read() {
  Fixture f;
  std::vector<detail::FileReadCompletion> reads;
  int callbacks = 0;
  auto reader = [&](off_t, std::size_t, detail::FileReadCompletion completion) {
    reads.push_back(std::move(completion));
  };
  auto done = [&](std::error_code error, std::uint64_t sent) {
    check(!error && sent == 3, "incorrect completion");
    ++callbacks;
  };
  f.scheduler->enqueue_file(WriteScheduler::prepare_async(0, 3, reader, done));
  f.scheduler->enqueue_file(WriteScheduler::prepare_async(0, 3, reader, done));
  f.scheduler->on_writable();
  std::thread worker([&] {
    reads[0]({}, "abc");
  });
  worker.join();
  check(!f.writable && callbacks == 0, "worker completion bypassed loop");
  f.pump();
  f.scheduler->on_writable();
  check(reads.size() == 2 && !f.writable, "second read was not requested");
  reads[0]({}, "BAD");
  f.pump();
  check(!f.writable, "stale read replaced next file");
  reads[1]({}, "xyz");
  check(f.finish() == "abcxyz" && callbacks == 2, "worker/stale completion corrupted data");
}

void reader_error_and_limits() {
  {
    Fixture f;
    int callbacks = 0;
    f.scheduler->enqueue_file(WriteScheduler::prepare_async(
        0, 3,
        [](off_t, std::size_t, detail::FileReadCompletion done) {
          done({}, "xx");
        },
        [&](std::error_code error, std::uint64_t sent) {
          check(error == std::errc::io_error && sent == 0, "short read was accepted");
          ++callbacks;
        }));
    f.scheduler->enqueue_memory("TAIL");
    f.scheduler->on_writable();
    f.pump();
    f.pump();
    check(f.failed == EIO && callbacks == 1 && f.receive().empty(), "short read did not fail FIFO");
  }
  {
    Fixture f;
    f.scheduler->enqueue_memory(std::string(1024 * 1024, 'x'));
    f.scheduler->enqueue_memory("overflow");
    check(f.failed == ENOBUFS && f.scheduler->empty() && !f.writable,
          "memory limit failed to cancel FIFO");
    check(f.scheduler->pending_memory_bytes() == 0, "failure leaked reservation");
  }
  {
    Fixture f;
    int callbacks = 0;
    f.scheduler->enqueue_file(WriteScheduler::prepare_async(
        0, 0,
        [](off_t, std::size_t, detail::FileReadCompletion) {
          throw std::runtime_error("empty file invoked reader");
        },
        [&](std::error_code error, std::uint64_t sent) {
          check(!error && sent == 0, "empty file failed");
          ++callbacks;
        }));
    f.scheduler->enqueue_memory("TAIL");
    check(callbacks == 0, "completion ran inline");
    check(f.finish() == "TAIL" && callbacks == 1, "empty file blocked FIFO");
  }
}
}  // namespace

int main() {
  try {
    mixed_fifo();
    waiting_cancel_and_late_completion();
    reader_cancels_synchronously();
    worker_completion_and_stale_read();
    reader_error_and_limits();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
