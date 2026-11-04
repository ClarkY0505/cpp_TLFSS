#include "ftp/transfer_task.h"

#include <cassert>
#include <stdexcept>

#include "common/net/event_loop.h"
#include "ftp/context.h"

namespace TLSS::FileStorage {
TransferContext::Current TransferContext::current() const {
    auto conn = connection.lock();
    auto original = session.lock();
    if (!conn || !original || !conn->connected()) return {};
    assert(conn->get_loop()->is_in_loop_thread());
    if (original->transfer_generation != generation ||
        original->login_state != LoginState::k_authenticated || original->user_id != user_id ||
        original->name != username || original->current_directory != directory)
        return {};
    return {std::move(conn), std::move(original)};
}

NET::TcpConnectionPtr TransferTask::current_connection() const {
    if (_cancelled) return {};
    auto current = _context.current();
    if (!current.first) return {};
    if (_stopping.load()) {
        current.first->force_close();
        return {};
    }
    return std::move(current.first);
}

void TransferTask::emit(TransferEvent event) {
    if (current_connection() && _notify) _notify(std::move(event));
}

void TransferTask::consume(NET::Buffer&) {
    throw std::logic_error("transfer is not receiving a file");
}

void TransferTask::acknowledge() {
    throw std::logic_error("transfer is not waiting for an acknowledgement");
}
}  // namespace TLSS::FileStorage
