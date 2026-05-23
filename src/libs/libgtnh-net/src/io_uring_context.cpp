#include <gtnh/net/io_uring_context.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <signal.h>

namespace gtnh::net {

bool IoUringContext::init(unsigned entries) {
    std::lock_guard<std::mutex> lock(sq_mutex_);
    if (initialized_) return true;

    unsigned flags = 0;
#ifdef IORING_SETUP_CQ_NODROP
    flags |= IORING_SETUP_CQ_NODROP;
#endif
#ifdef IORING_SETUP_COOP_TASKRUN
    flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_TASKRUN_FLAG
    flags |= IORING_SETUP_TASKRUN_FLAG;
#endif
    // DEFER_TASKRUN + SINGLE_ISSUER disabled on this kernel — causes -EEXIST
    // on io_uring_wait_cqe_timeout even on an idle ring (kernel's deferred
    // overflow handling returns EEXIST before CQEs are drained).
    // COOP_TASKRUN + TASKRUN_FLAG is sufficient for IPI-free completions.
#if 0
#ifdef IORING_SETUP_DEFER_TASKRUN
    flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
    flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#endif

    int rc = io_uring_queue_init(entries, &ring_, flags);
    if (rc < 0) {
        spdlog::error("IoUringContext: io_uring_queue_init failed: {}",
                      std::strerror(-rc));
        return false;
    }

    // Limit kernel IO worker threads — io_uring can spawn nproc*2 under
    // shapeshifting load; fix at 4 bound + 4 unbound for stability.
    unsigned workers[2] = {4, 4};
    io_uring_register_iowq_max_workers(&ring_, workers);

    initialized_ = true;
    running_.store(true, std::memory_order_release);
    poll_thread_ = std::thread(&IoUringContext::poll_loop, this);

    spdlog::debug("IoUringContext: single ring init ({} entries)", entries);
    return true;
}

void IoUringContext::shutdown() {
    running_.store(false, std::memory_order_release);
    // No NOP to wake the poll thread — with SINGLE_ISSUER + DEFER_TASKRUN
    // we must only submit from the poll thread.  The 50 ms wait_cqe timeout
    // guarantees the poll thread notices running_ == false within ~50 ms.

    if (poll_thread_.joinable()) {
        poll_thread_.join();
        spdlog::debug("IoUringContext: poll thread joined");
    }

    if (initialized_) {
        io_uring_queue_exit(&ring_);
        initialized_ = false;
        spdlog::debug("IoUringContext: ring exited");
    }
}

void IoUringContext::poll_loop() {
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &ss, nullptr);

    while (running_.load(std::memory_order_acquire)) {
        struct io_uring_cqe* cqe = nullptr;

        // Step 1: Drain existing CQEs first — with DEFER_TASKRUN the CQ must
        // have room BEFORE io_uring_submit, or the kernel returns -EEXIST.
        int ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret == 0) {
            unsigned head = 0, count = 0;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                ++count;
                if (on_cqe) on_cqe(cqe->res, cqe->user_data);
            }
            io_uring_cq_advance(&ring_, count);
            continue;
        }

        // Step 2: Submit pending SQEs (CQ has room now)
        {
            std::lock_guard<std::mutex> lock(sq_mutex_);
            io_uring_submit(&ring_);
        }

        // Step 3: Wait for CQE with 50ms timeout
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        int rc = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (rc == -ETIME) {
            continue;
        }
        if (rc < 0) {
            if (rc == -EINTR) continue;
            spdlog::error("IoUringContext: wait_cqe error: {}", std::strerror(-rc));
            break;
        }

        // Step 4: Process all available CQEs
        unsigned head = 0, count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++count;
            if (on_cqe) on_cqe(cqe->res, cqe->user_data);
        }
        io_uring_cq_advance(&ring_, count);
    }
}

io_uring_sqe* IoUringContext::get_sqe() {
    std::lock_guard<std::mutex> lock(sq_mutex_);
    return io_uring_get_sqe(&ring_);
}

void IoUringContext::submit() {
    std::lock_guard<std::mutex> lock(sq_mutex_);
    io_uring_submit(&ring_);
}

} // namespace gtnh::net