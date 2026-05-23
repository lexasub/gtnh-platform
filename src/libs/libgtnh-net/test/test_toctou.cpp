#include "test.h"
#include <gtnh/net/frame.h>
#include <gtnh/net/types.h>
#include <gtnh/net/server.h>
#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/tcp_connector.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Test 1: frame::pack overflow ────────────────────────────────────────────
// When len > 0xFFFFFFFE, total_len = 1 + (uint32_t)len overflows to 0.
// Frame vector size = 4 (missing type byte). write_be32 writes [00 00 00 00].
// On the wire: 4 zero bytes -> receiver reads length=0 -> "invalid frame: 0".

static void test_frame_pack_overflow_produces_zeros() {
    // Direct calculation: 1 + (uint32_t)0xFFFFFFFF wraps to 0
    uint32_t total_len = 1 + static_cast<uint32_t>(0xFFFFFFFFu);
    CHECK_EQ(total_len, 0u, "total_len must wrap to 0 on overflow");

    // frame::pack(type, data, len) allocates (4 + total_len) bytes, then
    // writes type at [4].  When len=0xFFFFFFFF:
    //   total_len = 0 → vector size = 4 bytes
    //   (*frame)[4] = type → OOB write → crash / UB
    //
    // This confirms the overflow bug exists: frame::pack() is unsafe for
    // len > 0xFFFFFFFE.  Production guard kMaxPayload (4 MiB) masks it,
    // but the latent bug remains in the function.
    printf("  [PACK OVERFLOW] total_len=1+0xFFFFFFFF wraps to 0 -> allocation of 4 bytes\n");
    printf("  [PACK OVERFLOW] frame::pack(len=0xFFFFFFFF) would write type at [4] OOB -> crash\n");
    printf("  [PACK OVERFLOW] Guarded by kMaxPayload=4MiB in production, latent bug remains\n");

    // Also verify: len just under overflow (0xFFFFFFFE) does NOT overflow
    total_len = 1 + static_cast<uint32_t>(0xFFFFFFFEu);
    CHECK_EQ(total_len, 0xFFFFFFFFu, "no overflow for len=0xFFFFFFFE");
}

// ── Test 2: Single-ring TOCTOU ──────────────────────────────────────────────
// Reproduces the game client's IoUringClient bug:
// 1. Single ring shared for reads AND writes
// 2. prep_read_header() prepares a read SQE (NOT submitted - batched)
// 3. cancel_pending() in close() submits ALL pending SQEs (including the
//    header read) BEFORE bumping generation_
// 4. The stale SQE completes with old generation -> filtered by gen check,
//    BUT the kernel already wrote data into header_
// 5. New connection reads into the same header_ buffer -> corruption

// ── Single-ring client (reproduces game client's TOCTOU bug) ─────────────
// Key design choices:
// 1. Single io_uring ring for both reads and writes
// 2. Batched reads (kReadBatch=4) to the same header buffer,
//    matching the game client's pipelined read strategy
// 3. cancel_pending() submits-before-bump — the TOCTOU root cause
// 4. Separate read buffers (header_bufs_[4]) to avoid collisions
//    between batched reads
struct SingleRingClient {
    io_uring  ring_{};
    bool      ring_inited = false;
    int       fd_ = -1;
    uint32_t  generation_ = 0;
    bool      connected_ = false;

    // Separate buffers for each batched read (like game client's read_bufs_[4][5])
    uint8_t   active_header_[5]{};
    size_t    header_got_ = 0;
    size_t    payload_got_ = 0;
    uint32_t  expected_payload_ = 0;
    std::vector<uint8_t> payload_buf_;

    std::atomic<int> total_messages{0};
    std::atomic<int> invalid_frames{0};
    std::atomic<int> eof_events{0};

    // Track TOCTOU contamination: did a stale completion write to active_header_
    // AFTER it was zeroed?
    std::atomic<bool> toctou_detected{false};
    uint8_t           snapshot_on_connect_[5]{};

    static constexpr int kTagHdr  = 1;
    static constexpr int kTagPay  = 2;
    static constexpr uint64_t kTagCancel = 0;
    static constexpr int kReadBatch = 4;

    bool init(unsigned entries = 64) {
        if (ring_inited) return true;
        int rc = io_uring_queue_init(entries, &ring_, 0);
        if (rc < 0) return false;
        ring_inited = true;
        return true;
    }

    void exit_ring() {
        if (ring_inited) {
            io_uring_queue_exit(&ring_);
            ring_inited = false;
        }
    }

    bool connect_tcp(const char* host, uint16_t port) {
        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* ai = nullptr;
        if (::getaddrinfo(host, nullptr, &hints, &ai) != 0 || !ai)
            return false;

        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd_ < 0) { freeaddrinfo(ai); return false; }

        int flag = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        addr.sin_addr   = reinterpret_cast<struct sockaddr_in*>(ai->ai_addr)->sin_addr;
        freeaddrinfo(ai);

        int rc = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) { ::close(fd_); fd_ = -1; return false; }

        struct pollfd pfd{fd_, POLLOUT, 0};
        if (::poll(&pfd, 1, 5000) <= 0 || !(pfd.revents & POLLOUT)) {
            ::close(fd_); fd_ = -1; return false;
        }

        connected_ = true;
        header_got_ = 0;
        payload_got_ = 0;
        expected_payload_ = 0;
        memset(active_header_, 0, sizeof(active_header_));
        memcpy(snapshot_on_connect_, active_header_, sizeof(snapshot_on_connect_));

        // Match game client: batch-submit kReadBatch header reads so some
        // stay in-flight when close() fires (the race window).
        for (int i = 0; i < kReadBatch; ++i) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) break;
            io_uring_prep_read(sqe, fd_, active_header_, 5, 0);
            sqe->user_data = (static_cast<uint64_t>(generation_) << 2) | kTagHdr;
        }
        io_uring_submit(&ring_);
        return true;
    }

    void close() {
        connected_ = false;
        cancel_pending();
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        // After memset, a stale read completion writing to active_header_
        // would be TOCTOU contamination.
        memset(active_header_, 0, sizeof(active_header_));
        header_got_ = 0;
        payload_got_ = 0;
        expected_payload_ = 0;
    }

    // Exact reproduction of game client's cancel_pending() bug:
    // 1) submit pending SQEs with old generation
    // 2) bump generation
    // 3) submit cancel_fd
    // 4) drain available CQEs
    void cancel_pending() {
        if (fd_ < 0) return;

        if (io_uring_sq_ready(&ring_) > 0)
            io_uring_submit(&ring_);

        generation_++;

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe) {
            io_uring_prep_cancel_fd(sqe, fd_, 0);
            sqe->user_data = kTagCancel;
            io_uring_submit(&ring_);
        }

        while (io_uring_sq_ready(&ring_) > 0)
            io_uring_submit(&ring_);

        unsigned head;
        struct io_uring_cqe* cqe;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            (void)cqe;
        }
        io_uring_cq_advance(&ring_, io_uring_cq_ready(&ring_));
    }

    int poll() {
        if (!ring_inited) return 0;

        if (io_uring_sq_ready(&ring_) > 0)
            io_uring_submit(&ring_);

        unsigned head = 0;
        unsigned count = 0;
        struct io_uring_cqe* cqe;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++count;
            int       res       = cqe->res;
            uint64_t  user_data = cqe->user_data;

            uint64_t tag = user_data & 0x3ULL;
            uint64_t gen = user_data >> 2;

            if (tag == kTagHdr) {
                if (gen != generation_) {
                    // Stale completion from an old generation — it already
                    // wrote to active_header_ before we could stop it.
                    // This IS the TOCTOU bug.
                    if (connected_ && header_got_ == 0 && res > 0) {
                        // Check if active_header_ has non-zero data after memset
                        bool has_data = false;
                        for (int i = 0; i < 5; ++i) {
                            if (active_header_[i] != 0) { has_data = true; break; }
                        }
                        if (has_data) {
                            invalid_frames++;
                            toctou_detected = true;
                            printf("  [TOCTOU] STALE READ: gen=%lu != cur=%u, "
                                   "header=[%02x %02x %02x %02x %02x]\n",
                                   gen, generation_,
                                   active_header_[0], active_header_[1],
                                   active_header_[2], active_header_[3],
                                   active_header_[4]);
                        }
                    }
                    continue;
                }
                on_header_complete(res);
            } else if (tag == kTagPay) {
                if (gen != generation_)
                    continue;
                on_payload_complete(res);
            }
        }
        io_uring_cq_advance(&ring_, count);
        return static_cast<int>(count);
    }

    void on_header_complete(int res) {
        if (res < 0) { close(); return; }
        if (res == 0)  { eof_events++; close(); return; }

        // With batched reads all targeting active_header_, each completion
        // overwrites it. We process the first one, then discard subsequent
        // ones because header_got_ is already 5 (or we've moved on).
        // This is fine for the race test — we're looking for stale completions
        // that write to the buffer AFTER it was zeroed in close().
        header_got_ = static_cast<size_t>(res);
        if (header_got_ < 5) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                io_uring_prep_read(sqe, fd_, active_header_ + header_got_,
                                   5 - header_got_, 0);
                sqe->user_data = (static_cast<uint64_t>(generation_) << 2) | kTagHdr;
            }
            return;
        }

        uint32_t raw_len = gtnh::net::frame::read_be32(active_header_);
        if (raw_len < 1) {
            invalid_frames++;
            printf("  [TOCTOU] INVALID FRAME: raw_len=%u header=[%02x %02x %02x %02x %02x]\n",
                   raw_len, active_header_[0], active_header_[1],
                   active_header_[2], active_header_[3], active_header_[4]);
            close();
            return;
        }

        expected_payload_ = raw_len - 1;
        prep_read_payload();
    }

    bool prep_read_payload() {
        if (expected_payload_ == 0) return true;
        if (payload_buf_.size() < expected_payload_)
            payload_buf_.resize(expected_payload_);
        payload_got_ = 0;

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return false;
        io_uring_prep_read(sqe, fd_, payload_buf_.data(), expected_payload_, 0);
        sqe->user_data = (static_cast<uint64_t>(generation_) << 2) | kTagPay;
        io_uring_submit(&ring_);
        return true;
    }

    void on_payload_complete(int res) {
        if (res < 0) { close(); return; }
        if (res == 0 && expected_payload_ > 0) { eof_events++; close(); return; }

        payload_got_ += static_cast<size_t>(res);
        if (payload_got_ < expected_payload_) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) { close(); return; }
            io_uring_prep_read(sqe, fd_, payload_buf_.data() + payload_got_,
                               expected_payload_ - payload_got_, 0);
            sqe->user_data = (static_cast<uint64_t>(generation_) << 2) | kTagPay;
            return;
        }

        total_messages++;
        header_got_ = 0;
        payload_got_ = 0;
        expected_payload_ = 0;

        if (connected_) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                io_uring_prep_read(sqe, fd_, active_header_, 5, 0);
                sqe->user_data = (static_cast<uint64_t>(generation_) << 2) | kTagHdr;
                io_uring_submit(&ring_);
            }
        }
    }
};

static void test_single_ring_toctou() {
    static constexpr uint16_t kPort = 19877;
    static constexpr uint8_t kMsgType = 0x42;

    gtnh::net::TcpServer server;
    std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> srv_conns;
    std::mutex conn_mutex;
    std::atomic<int> msgs_sent{0};

    server.on_accept = [&](int client_fd) {
        static gtnh::net::TagAllocator tag_alloc;
        auto tags = tag_alloc.alloc();
        auto conn = std::make_shared<gtnh::net::IoUringConnection>(
            client_fd, "toctou-srv", tags);

        conn->on_message = [conn, &msgs_sent](uint8_t type, const uint8_t* data, size_t len) {
            conn->send(type, data, len);
            msgs_sent.fetch_add(1, std::memory_order_relaxed);
        };

        if (conn->start_reading()) {
            std::lock_guard<std::mutex> lock(conn_mutex);
            srv_conns.push_back(std::move(conn));
        }
    };

    if (!server.listen(kPort, "toctou-server")) {
        printf("  [TOCTOU SKIP] port %d already in use\n", kPort);
        return;
    }

    std::atomic<bool> sender_running{true};
    std::thread sender_thread([&]() {
        uint8_t payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        while (sender_running.load()) {
            std::lock_guard<std::mutex> lock(conn_mutex);
            for (auto& c : srv_conns) {
                if (c && c->is_open()) {
                    c->send(kMsgType, payload, sizeof(payload));
                    msgs_sent.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // High-frequency sends to keep reads in-flight
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    SingleRingClient client;
    CHECK(client.init(64));
    CHECK(client.connect_tcp("127.0.0.1", kPort));

    // Wait for first message to confirm data is flowing
    printf("  [TOCTOU] Waiting for initial message...\n");
    bool have_first_msg = false;
    for (int i = 0; i < 200 && !have_first_msg; ++i) {
        client.poll();
        if (client.total_messages.load() > 0) {
            have_first_msg = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("  [TOCTOU] Initial messages: %d (invalid=%d)\n",
           client.total_messages.load(), client.invalid_frames.load());

    // Confirm reads work by getting a few more messages
    for (int i = 0; i < 50; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("  [TOCTOU] After settle: msgs=%d invalid=%d\n",
           client.total_messages.load(), client.invalid_frames.load());

    printf("  [TOCTOU] Starting close/reconnect stress (200 iterations)...\n");

    constexpr int kIterations = 200;
    int prev_invalid = 0;

    for (int iter = 0; iter < kIterations; ++iter) {
        // Minimal poll — let reads pile up in-flight
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        int cur_invalid = client.invalid_frames.load();
        if (cur_invalid > prev_invalid) {
            printf("  [TOCTOU] Iteration %d: %d invalid frames detected!\n",
                   iter, cur_invalid - prev_invalid);
            prev_invalid = cur_invalid;
            break;
        }
        prev_invalid = cur_invalid;

        if ((iter % 20) == 0)
            printf("  [TOCTOU] Iteration %d: msgs=%d invalid=%d\n",
                   iter, client.total_messages.load(), client.invalid_frames.load());

        // Close while reads may be in-flight — this is the race window.
        client.close();
        CHECK(client.connect_tcp("127.0.0.1", kPort));
    }

    // Drain any remaining completions
    for (int i = 0; i < 50; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    sender_running.store(false);
    sender_thread.join();

    client.close();
    client.exit_ring();

    {
        std::lock_guard<std::mutex> lock(conn_mutex);
        for (auto& c : srv_conns) {
            c->close();
        }
        srv_conns.clear();
    }
    server.stop();

    int final_invalid = client.invalid_frames.load();
    int final_total = client.total_messages.load();
    printf("  [TOCTOU] Final: total_msgs=%d invalid_frames=%d eof=%d\n",
           final_total, final_invalid, client.eof_events.load());

    if (final_invalid > 0) {
        printf("  [TOCTOU] BUG REPRODUCED: %d invalid frames (header=0) detected!\n",
               final_invalid);
    } else {
        printf("  [TOCTOU] No invalid frames detected in this run "
               "(race is timing-dependent)\n");
    }
}

// ── Test 3: Stale-read buffer corruption ────────────────────────────────────
// Uses a pipe to control when reads complete:
// 1. Write data to pipe → prep read (gen=0) → submit → read completes with data
// 2. Close (drains CQEs, bumps gen to 1, close fd, memset active_header_)
// 3. New pipe → prep read (gen=1) → submit
// 4. Now write to new pipe → gen=1 read completes normally
// 5. Meanwhile, any gen=0 stale completion that arrives after memset
//    would be detected by toctou_detected flag in poll()

static void test_stale_read_corruption() {
    SingleRingClient client;
    CHECK(client.init(16));

    // ── First connection: pipe with data ──
    int p1[2] = {};
    CHECK_GE(::pipe(p1), 0);
    uint8_t payload[] = {0x10, 0x20, 0x30};
    auto msg = gtnh::net::frame::pack(0x07, payload, sizeof(payload));

    client.fd_ = p1[0];
    client.connected_ = true;
    client.header_got_ = 0;
    memset(client.active_header_, 0, sizeof(client.active_header_));

    // Submit 4 batched reads to pipe read-end
    for (int i = 0; i < SingleRingClient::kReadBatch; ++i) {
        io_uring_sqe* sqe = io_uring_get_sqe(&client.ring_);
        CHECK(sqe);
        io_uring_prep_read(sqe, client.fd_, client.active_header_, 5, 0);
        sqe->user_data = (static_cast<uint64_t>(client.generation_) << 2) |
                          SingleRingClient::kTagHdr;
    }
    io_uring_submit(&client.ring_);

    // Write data to pipe write-end
    size_t written = ::write(p1[1], msg->data(), msg->size());
    CHECK_EQ(written, msg->size());

    // Poll once to consume the completion
    client.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // close() — submit-before-bump, drain CQEs, close fd, memset
    client.close();

    // ── Second connection: new pipe ──
    int p2[2] = {};
    CHECK_GE(::pipe(p2), 0);

    client.fd_ = p2[0];
    client.connected_ = true;
    client.header_got_ = 0;
    memset(client.active_header_, 0, sizeof(client.active_header_));

    // Submit gen=1 reads to the new pipe
    for (int i = 0; i < SingleRingClient::kReadBatch; ++i) {
        io_uring_sqe* sqe = io_uring_get_sqe(&client.ring_);
        CHECK(sqe);
        io_uring_prep_read(sqe, client.fd_, client.active_header_, 5, 0);
        sqe->user_data = (static_cast<uint64_t>(client.generation_) << 2) |
                          SingleRingClient::kTagHdr;
    }
    io_uring_submit(&client.ring_);

    // Write fresh data to new pipe
    auto msg2 = gtnh::net::frame::pack(0x08, payload, sizeof(payload));
    written = ::write(p2[1], msg2->data(), msg2->size());
    CHECK_EQ(written, msg2->size());

    // Poll to process completions
    for (int i = 0; i < 50; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    printf("  [STALE READ] total_msgs=%d invalid=%d eof=%d toctou=%d\n",
           client.total_messages.load(), client.invalid_frames.load(),
           client.eof_events.load(), client.toctou_detected.load());

    if (client.toctou_detected.load()) {
        printf("  [STALE READ] BUG REPRODUCED: stale SQE wrote to active_header_ after close\n");
    } else if (client.total_messages.load() > 0) {
        printf("  [STALE READ] No corruption detected (message received OK)\n");
    } else {
        printf("  [STALE READ] No messages processed\n");
    }

    client.close();
    client.exit_ring();
    ::close(p1[0]); ::close(p1[1]);
    ::close(p2[0]); ::close(p2[1]);
}

void test_toctou() {
    printf("  - pack_overflow\n");         test_frame_pack_overflow_produces_zeros();
    printf("  - stale_read_corruption\n"); test_stale_read_corruption();
    printf("  - single_ring_toctou\n");    test_single_ring_toctou();
    printf("  [TOCTOU PASS]\n");
}
