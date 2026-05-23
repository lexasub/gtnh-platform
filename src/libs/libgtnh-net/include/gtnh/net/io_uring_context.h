#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include <liburing.h>

namespace gtnh::net {

class IoUringContext {
public:
    IoUringContext() = default;
    ~IoUringContext() { shutdown(); }

    IoUringContext(const IoUringContext&) = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;
    IoUringContext(IoUringContext&&) = delete;
    IoUringContext& operator=(IoUringContext&&) = delete;

    bool init(unsigned entries = 256);
    void shutdown();

    io_uring_sqe* get_sqe();
    void submit();
    std::move_only_function<void(int res, uint64_t user_data)> on_cqe;

private:
    void poll_loop();

    io_uring ring_{};

    std::mutex sq_mutex_;

    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    bool initialized_ = false;
};

} // namespace gtnh::net