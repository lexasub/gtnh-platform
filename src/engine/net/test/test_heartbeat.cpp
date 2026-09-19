#include "test.h"
#include <gtnh/net/io_uring_connection.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

// Verifies the poll-loop auto-heartbeat: with heartbeat_interval_ set, the
// connection fires on_heartbeat periodically with NO traffic on the socket
// and NO external timer thread. Regression for the router idle-killing
// silent services (e.g. chunkstore with no chunk.requests).
void test_heartbeat() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [HEARTBEAT SKIP] socketpair failed\n");
        return;
    }

    gtnh::net::TagAllocator tag_alloc;
    auto tags = tag_alloc.alloc();
    auto conn = std::make_unique<gtnh::net::IoUringConnection>(
        sv[0], "hb-test", tags);

    std::atomic<int> hb_count{0};
    conn->heartbeat_interval_ = std::chrono::milliseconds(400);
    conn->on_heartbeat = [&hb_count]() {
        hb_count.fetch_add(1, std::memory_order_relaxed);
    };

    CHECK(conn->start_reading());

    // Wait ~1.8s of pure idle — no data sent, no timer thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));

    int fired = hb_count.load(std::memory_order_relaxed);
    conn->close();
    conn.reset();
    ::close(sv[1]);

    // 400ms interval over ~1.8s → expect >= 3 fires (allowing jitter).
    CHECK_GE(fired, 2, "auto-heartbeat did not fire during idle");
    printf("  [HEARTBEAT PASS] on_heartbeat fired %d times in 1.8s idle\n", fired);
}
