#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/frame.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gtnh::net {

// Max write SQEs to submit per io_uring_submit call. Higher values batch more
// writes into a single syscall but increase per-SQE latency.
constexpr unsigned kWriteBatchLimit = 32; // TODO bug! for using 128, we need in IoUringConnection::send_raw run start_next_writes any time, need validate that it good solution and use, or find bug

std::atomic<uint64_t> IoUringConnection::next_generation_{1};

IoUringConnection::IoUringConnection(int fd, const char* name, ConnectionTags tags)
    : fd_(fd), name_(name)
    , generation_(next_generation_++)
    , tag_hdr_(tags.hdr_tag), tag_pay_(tags.pay_tag), tag_write_(tags.write_base)
{}

IoUringConnection::~IoUringConnection() {
    close();
    if (poll_thread_.joinable() &&
        poll_thread_.get_id() != std::this_thread::get_id()) {
        poll_thread_.join();
    }
}

bool IoUringConnection::init_read_ring_internal(unsigned entries) {
    unsigned flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
#ifdef IORING_SETUP_CQ_NODROP
    flags |= IORING_SETUP_CQ_NODROP;
#endif

    int rc = io_uring_queue_init(entries, &ring_, flags);
    if (rc < 0) {
        spdlog::error("{}: io_uring_queue_init (read ring) failed: {}", name_, std::strerror(-rc));
        return false;
    }
    unsigned workers[2] = {4, 4};
    io_uring_register_iowq_max_workers(&ring_, workers);
    read_ring_inited_.store(true, std::memory_order_release);
    return true;
}

bool IoUringConnection::init_write_ring(unsigned entries) {

    // Write ring — multi-submitter: poll thread (on_write_complete) + external
    // threads (send/send_raw) both call start_next_writes().  Cannot use
    // SINGLE_ISSUER/DEFER_TASKRUN; use COOP_TASKRUN to avoid IPI noise.
    unsigned wflags = IORING_SETUP_SQPOLL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;

    int rc = io_uring_queue_init(entries, &ring_write_, wflags);
    if (rc < 0) {
        spdlog::warn("{}: SQPOLL init failed (rc={}: {}), fallback to regular",
                     name_, rc, std::strerror(-rc));
        wflags &= ~static_cast<unsigned>(IORING_SETUP_SQPOLL);
        rc = io_uring_queue_init(entries, &ring_write_, wflags);
    } else {
        spdlog::info("{}: SQPOLL write ring OK (entries={})", name_, entries);
    }
    if (rc < 0) {
        spdlog::error("{}: io_uring_queue_init (write ring) failed: {}", name_, std::strerror(-rc));
        return false;
    }
    unsigned wworkers[2] = {4, 4};
    io_uring_register_iowq_max_workers(&ring_write_, wworkers);

    write_ring_inited_.store(true, std::memory_order_release);
    spdlog::debug("{}: write ring init ({})", name_, entries);
    return true;
}

void IoUringConnection::exit_rings() {
    if (read_ring_inited_.exchange(false, std::memory_order_acq_rel)) {
        io_uring_queue_exit(&ring_);
    }
    if (write_ring_inited_.exchange(false, std::memory_order_acq_rel)) {
        io_uring_queue_exit(&ring_write_);
    }
}

bool IoUringConnection::start_reading() {
    if (!init_write_ring(kDefaultRingEntries)) return false;

    running_.store(true, std::memory_order_release);

    // Poll thread inits the read ring — the poll thread becomes the single
    // issuer, so DEFER_TASKRUN + SINGLE_ISSUER work correctly (no EEXIST).
    auto read_ready = std::make_shared<std::promise<bool>>();
    auto fut = read_ready->get_future();
    poll_thread_ = std::thread([this, read_ready]() mutable {
        poll_loop(std::move(read_ready));
    });

    if (!fut.get()) {
        // Read ring init failed — shut down
        running_.store(false, std::memory_order_release);
        poll_thread_.join();
        io_uring_queue_exit(&ring_write_);
        int old_fd = fd_.exchange(-1, std::memory_order_acq_rel);
        if (old_fd >= 0) {
            ::shutdown(old_fd, SHUT_RDWR);
            ::close(old_fd);
        }
        return false;
    }
    return true;
}

void IoUringConnection::close() {
    int expected_fd = fd_.load(std::memory_order_acquire);
    if (expected_fd < 0) return;

    shutting_down_.store(true, std::memory_order_release);

    bool need_finalize = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.clear();

        if (in_flight_writes_.empty()) {
            need_finalize = true;
        } else {
            close_pending_.store(true, std::memory_order_release);
        }
    }

    if (!need_finalize) return;

    // No writes in flight — stop the poll thread and finalize.
    // If called from the poll thread itself, close the fd immediately so
    // is_open() returns false (the loop exit will still call cleanup()).
    // Otherwise, trigger and wait.
    if (poll_thread_.get_id() == std::this_thread::get_id()) {
        int old_fd = fd_.exchange(-1, std::memory_order_acq_rel);
        if (old_fd >= 0) {
            ::shutdown(old_fd, SHUT_RDWR);
            ::close(old_fd);
        }
        running_.store(false, std::memory_order_release);
        return;
    }

    running_.store(false, std::memory_order_release);
    // No NOP to wake the poll thread — it has a 50 ms timeout so it will
    // notice running_ == false within that window. Also, the poll thread's
    // exit handler may call cleanup() which frees the ring memory; submitting
    // a NOP here would access the freed ring and crash.
    if (poll_thread_.joinable()) poll_thread_.join();
    // After join the poll thread has finished. Its exit handler may have
    // already called cleanup() (if the connection self-closed, e.g. peer
    // disconnect detected in on_cqe).  Only call cleanup() ourselves if it
    // hasn't run yet (fd_ would still be >= 0).
    if (fd_.load(std::memory_order_acquire) >= 0) {
        cleanup();
    }
}

void IoUringConnection::cleanup() {
    close_pending_.store(false, std::memory_order_release);

    exit_rings();

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.clear();
        in_flight_writes_.clear();
    }

    int fd = fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        spdlog::debug("{}: fd closed", name_);
    }

    read_phase_ = ReadPhase::HEADER;
    header_got_ = 0; payload_got_ = 0; expected_payload_ = 0;
    consecutive_bad_headers_ = 0;

    auto cb = std::move(on_closed);
    on_closed = nullptr;
    if (cb) cb();
}

// ── Poll loop ──────────────────────────────────────────────────────────────

void IoUringConnection::poll_loop(std::shared_ptr<std::promise<bool>> read_ready) {
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &ss, nullptr);

    // Use poll()+read() instead of io_uring for reads — on this kernel
    // io_uring reads on an empty TCP socket spuriously complete with res=5
    // and a zero-filled buffer.
    // Write ring still uses io_uring.
    read_ready->set_value(true);

    io_uring_cqe* cqe = nullptr;
    while (running_.load(std::memory_order_acquire)) {
        {
            unsigned head = 0, wcount = 0;
            io_uring_for_each_cqe(&ring_write_, head, cqe) {
                ++wcount;
                on_write_complete(cqe->res, cqe->user_data);
            }
            if (wcount > 0) io_uring_cq_advance(&ring_write_, wcount);
        }

        if (!running_.load(std::memory_order_acquire)) break;

        {
            struct pollfd pfd{fd(), POLLIN, 0};
            int pret = ::poll(&pfd, 1, 50);
            if (pret < 0) {
                if (errno == EINTR) continue;
                spdlog::error("{}: poll error: {}", name_, std::strerror(errno));
                break;
            }
            if (pret == 0) continue;
        }

        {
            ssize_t n = ::read(fd(), header_ + header_got_, 5 - header_got_);
            if (n <= 0) {
                if (n == 0) {
                    spdlog::info("{}: peer closed (read=0)", name_);
                    close();
                    goto poll_exit;
                }
                if (errno == EINTR || errno == EAGAIN) {
                    // Non-blocking read after spurious POLLIN wakeup —
                    // retry next iteration instead of closing.
                    spdlog::debug("{}: spurious POLLIN after poll(), retrying ({})", name_,
                                   errno == EINTR ? "EINTR" : "EAGAIN");
                    continue;
                }
                spdlog::error("{}: read error: {}", name_, std::strerror(errno));
                close();
                goto poll_exit;
            }
            header_got_ += static_cast<size_t>(n);
            if (header_got_ < 5) continue;
        }

        uint32_t raw_len = frame::read_be32(header_);
        if (raw_len < 1) {
            spdlog::warn("{}: bad header: raw_len={} header_hex={:02x}{:02x}{:02x}{:02x}{:02x}",
                          name_, raw_len,
                          header_[0], header_[1], header_[2], header_[3], header_[4]);
            // Grace period: don't count or close — kernel may emit stale zero-reads
            // right after connection on this kernel version.
            if (grace_elapsed()) {
                if (++consecutive_bad_headers_ > 3) {
                    spdlog::error("{}: too many bad headers, closing", name_);
                    close();
                    goto poll_exit;
                }
            }
            header_got_ = 0;
            continue;
        }
        consecutive_bad_headers_ = 0;

        header_got_ = 0;

        if (raw_len == 1) {
            uint8_t mt = header_[4];
            if (on_message) on_message(mt, nullptr, 0);
            continue;
        }

        uint32_t payload_len = raw_len - 1;
        if (payload_len > kMaxPayload) {
            spdlog::error("{}: payload too large: {} (max {})", name_, payload_len, kMaxPayload);
            close();
            goto poll_exit;
        }

        if (payload_buf_.size() < payload_len)
            payload_buf_.resize(payload_len);

        size_t got = 0;
        while (got < payload_len) {
            ssize_t n = ::read(fd(), payload_buf_.data() + got, payload_len - got);
            if (n <= 0) {
                if (n == 0) {
                    spdlog::info("{}: peer closed during payload read", name_);
                    close();
                    goto poll_exit;
                }
                if (errno == EINTR || errno == EAGAIN) {
                    // Spurious wakeup — retry instead of closing.
                    spdlog::debug("{}: spurious POLLIN during payload read, retrying ({})", name_,
                                   errno == EINTR ? "EINTR" : "EAGAIN");
                    continue;
                }
                spdlog::error("{}: payload read error: {}", name_, std::strerror(errno));
                close();
                goto poll_exit;
            }
            got += static_cast<size_t>(n);
        }

        {
            uint8_t mt = header_[4];
            if (on_message) on_message(mt, payload_buf_.data(), got);
        }
    }

poll_exit:
    cleanup();
}

// ── Read path ──────────────────────────────────────────────────────────────

bool IoUringConnection::prep_read_header() {
    int read_fd = fd();
    if (read_fd < 0) return false;

    std::lock_guard<std::mutex> lock(sq_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::warn("{}: SQ full (read hdr)", name_);
        return false;
    }
    io_uring_prep_read(sqe, read_fd, header_ + header_got_, 5 - header_got_, 0);
    sqe->user_data = encode_user_data(generation_, tag_hdr_);
    io_uring_submit(&ring_);
    return true;
}

bool IoUringConnection::prep_read_payload() {
    int read_fd = fd();
    if (read_fd < 0) return false;

    if (expected_payload_ == 0) {
        on_read_payload_complete(0);
        return true;
    }

    if (payload_buf_.size() < expected_payload_)
        payload_buf_.resize(expected_payload_);
    payload_got_ = 0;

    std::lock_guard<std::mutex> lock(sq_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        spdlog::error("{}: SQ full (read pay)", name_);
        return false;
    }
    io_uring_prep_read(sqe, read_fd, payload_buf_.data(), expected_payload_, 0);
    sqe->user_data = encode_user_data(generation_, tag_pay_);
    io_uring_submit(&ring_);
    return true;
}

bool IoUringConnection::on_cqe(int res, uint64_t user_data) {
    uint64_t raw_tag = decode_tag(user_data);
    uint64_t gen = decode_generation(user_data);

    if (gen != generation_) return false;

    if (res < 0) {
        if (res != -ECANCELED)
            spdlog::error("{}: io error: {}", name_, std::strerror(-res));
        close(); return true;
    }
    if (res == 0) {
        spdlog::info("{}: peer closed", name_);
        close(); return true;
    }

    if (raw_tag == tag_hdr_) {
        on_read_header_complete(res); return true;
    }
    if (raw_tag == tag_pay_) {
        on_read_payload_complete(res); return true;
    }

    spdlog::trace("{}: unexpected read CQE tag={}", name_, raw_tag);
    return false;
}

void IoUringConnection::on_read_header_complete(int res) {
    header_got_ += static_cast<size_t>(res);
    if (header_got_ < 5) {
        if (!prep_read_header()) close();
        return;
    }

    uint32_t raw_len = frame::read_be32(header_);
    if (raw_len < 1) {
        spdlog::warn("{}: bad header: raw_len={} header_hex={:02x}{:02x}{:02x}{:02x}{:02x} "
                      "header_got_={} res={} fd={} running={} shutting_down={}",
                      name_, raw_len,
                      header_[0], header_[1], header_[2], header_[3], header_[4],
                      header_got_, res, fd(),
                      running_.load(std::memory_order_relaxed),
                      shutting_down_.load(std::memory_order_relaxed));
        // Grace period: don't count or close — kernel may emit stale zero-reads
        // right after connection on this kernel version.
        if (grace_elapsed()) {
            if (++consecutive_bad_headers_ > 3) {
                spdlog::error("{}: too many bad headers, closing", name_);
                close();
                return;
            }
        }
        // Skip the bad 5 bytes and continue reading
        header_got_ = 0;
        if (!prep_read_header()) close();
        return;
    }
    consecutive_bad_headers_ = 0;

    expected_payload_ = raw_len - 1;
    if (expected_payload_ > kMaxPayload) {
        spdlog::error("{}: payload too large: {} (max {})", name_, expected_payload_, kMaxPayload);
        close(); return;
    }

    read_phase_ = ReadPhase::PAYLOAD;
    if (!prep_read_payload()) close();
}

void IoUringConnection::on_read_payload_complete(int res) {
    uint8_t mt = header_[4];

    if (expected_payload_ == 0) {
        read_phase_ = ReadPhase::HEADER;
        header_got_ = 0; payload_got_ = 0; expected_payload_ = 0;

        if (mt == 0) {
            spdlog::warn("{}: bad header (type=0) at payload stage, skipping", name_);
            if (++consecutive_bad_headers_ > 3) {
                spdlog::error("{}: too many bad headers, closing", name_);
                close();
                return;
            }
            if (!prep_read_header()) close();
            return;
        }

        if (on_message) on_message(mt, nullptr, 0);
        if (!prep_read_header()) close();
        return;
    }

    payload_got_ += static_cast<size_t>(res);
    if (payload_got_ < expected_payload_) {
        // Partial read — prep another read for remaining bytes
        int read_fd = fd();
        if (read_fd < 0) { close(); return; }

        std::lock_guard<std::mutex> lock(sq_mutex_);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) { spdlog::error("{}: SQ full (partial pay)", name_); close(); return; }
        io_uring_prep_read(sqe, read_fd, payload_buf_.data() + payload_got_,
                           expected_payload_ - payload_got_, 0);
        sqe->user_data = encode_user_data(generation_, tag_pay_);
        io_uring_submit(&ring_);
        return;
    }

    if (mt == 0) {
        spdlog::warn("{}: bad header (type=0) with payload, skipping", name_);
        if (++consecutive_bad_headers_ > 3) {
            spdlog::error("{}: too many bad headers, closing", name_);
            close();
            return;
        }
        read_phase_ = ReadPhase::HEADER;
        header_got_ = 0; payload_got_ = 0; expected_payload_ = 0;
        if (!prep_read_header()) close();
        return;
    }

    const uint8_t* payload = payload_buf_.data();
    size_t plen = payload_got_;
    read_phase_ = ReadPhase::HEADER;
    header_got_ = 0; payload_got_ = 0; expected_payload_ = 0;
    if (on_message) on_message(mt, payload, plen);
    if (!prep_read_header()) close();
}

// ── Write path ─────────────────────────────────────────────────────────────

void IoUringConnection::send(uint8_t type, const uint8_t* data, size_t len) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    if (len > kMaxPayload) {
        spdlog::error("{}: send size too large: {} (max {})", name_, len, kMaxPayload);
        return;
    }
    auto frame = frame::pack(type, data, len);
    send_raw(std::move(frame));
}

void IoUringConnection::send_raw(std::shared_ptr<std::vector<uint8_t>> frame) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    auto op = std::make_unique<WriteOp>(WriteOp{std::move(frame), 0});
    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.push_back(std::move(op));
        need_start = in_flight_writes_.empty();
    }
    if (need_start) start_next_writes();
}

void IoUringConnection::start_next_writes() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_queue_.empty()) return;
    if (shutting_down_.load(std::memory_order_acquire)) {
        write_queue_.clear();
        return;
    }

    int write_fd = fd();
    if (write_fd < 0) {
        write_queue_.clear();
        return;
    }

    unsigned batch_count = 0;
    while (!write_queue_.empty() && batch_count < kWriteBatchLimit) {
        io_uring_sqe* sqe;
        {
            std::lock_guard<std::mutex> sq_lock(sq_mutex_write_);
            sqe = io_uring_get_sqe(&ring_write_);
        }
        if (!sqe) {
            spdlog::warn("{}: SQ full, {} writes queued", name_, write_queue_.size());
            break;
        }

        auto op = std::move(write_queue_.front());
        write_queue_.pop_front();
        ++batch_count;

        uint64_t seq = next_write_seq_++;
        io_uring_prep_write(sqe, write_fd, op->frame->data(),
                            op->frame->size(), 0);
        sqe->user_data = encode_user_data(generation_, tag_write_ + seq);
        in_flight_writes_[seq] = std::move(op);
    }

    if (batch_count > 0) {
        std::lock_guard<std::mutex> sq_lock(sq_mutex_write_);
        io_uring_submit(&ring_write_);
    }
}

void IoUringConnection::on_write_complete(int res, uint64_t user_data) {
    uint64_t seq = decode_tag(user_data) - tag_write_;

    std::unique_lock<std::mutex> lock(write_mutex_);
    auto it = in_flight_writes_.find(seq);
    if (it == in_flight_writes_.end()) {
        spdlog::trace("{}: write completion for unknown seq={} (close race)", name_, seq);
        return;
    }

    auto* op = it->second.get();
    op->offset += static_cast<size_t>(res);
    if (op->offset < op->frame->size()) {
        // Partial write — re-submit remaining bytes
        int write_fd = fd();
        if (write_fd < 0) {
            in_flight_writes_.erase(it);
            lock.unlock();
            spdlog::warn("{}: partial write after close", name_);
            return;
        }
        lock.unlock();

        {
            std::lock_guard<std::mutex> sq_lock(sq_mutex_write_);
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_write_);
            if (!sqe) {
                spdlog::error("{}: SQ full (partial write)", name_);
                close();
                return;
            }
            io_uring_prep_write(sqe, write_fd, op->frame->data() + op->offset,
                               op->frame->size() - op->offset, 0);
            sqe->user_data = user_data;
            io_uring_submit(&ring_write_);
        }
        return;
    }

    in_flight_writes_.erase(it);
    bool drained = in_flight_writes_.empty();
    bool need_finalize = drained && close_pending_.load(std::memory_order_acquire);
    lock.unlock();

    if (need_finalize) {
        // Signal poll loop to exit — exit handler will call cleanup().
        // Don't call cleanup() here: we're iterating write ring CQEs and
        // exit_rings() would free the ring struct (use-after-free in the
        // for_each macro and io_uring_cq_advance below).
        running_.store(false, std::memory_order_release);
        return;
    } else if (drained) {
        start_next_writes();
    }
}

} // namespace gtnh::net
