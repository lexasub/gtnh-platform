#include "test.h"
#include <gtnh/net/io_uring_context.h>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>

static void test_init_shutdown() {
    gtnh::net::IoUringContext ctx;
    CHECK(ctx.init(64));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ctx.shutdown();
    CHECK(true); // no crash = pass
}

static void test_double_init() {
    gtnh::net::IoUringContext ctx;
    CHECK(ctx.init(64));
    CHECK(ctx.init(64)); // second init should be no-op
    ctx.shutdown();
    CHECK(true);
}

static void test_init_shutdown_no_poll() {
    // init + immediate shutdown (no time for poll loop to run)
    gtnh::net::IoUringContext ctx;
    CHECK(ctx.init(64));
    ctx.shutdown();
    CHECK(true);
}

static void test_double_shutdown() {
    gtnh::net::IoUringContext ctx;
    CHECK(ctx.init(64));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ctx.shutdown();
    ctx.shutdown(); // second shutdown should be safe
    CHECK(true);
}

static void test_on_cqe_invocation() {
    gtnh::net::IoUringContext ctx;
    std::atomic<int> cqe_count{0};

    ctx.on_cqe = [&](int res, [[maybe_unused]] uint64_t user_data) {
        if (res >= 0) cqe_count.fetch_add(1, std::memory_order_relaxed);
    };

    CHECK(ctx.init(64));

    // Submit a NOP to trigger a CQE
    {
        auto* sqe = ctx.get_sqe();
        CHECK_NE(sqe, nullptr);
        io_uring_prep_nop(sqe);
        sqe->user_data = 42;
        ctx.submit();
    }

    // Wait for CQE to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(cqe_count.load(std::memory_order_relaxed) >= 1);

    ctx.shutdown();
}

void test_context() {
    printf("  - init_shutdown\n");         test_init_shutdown();
    printf("  - double_init\n");           test_double_init();
    printf("  - init_shutdown_no_poll\n"); test_init_shutdown_no_poll();
    printf("  - double_shutdown\n");       test_double_shutdown();
    printf("  - on_cqe_invocation\n");     test_on_cqe_invocation();
    printf("  [CONTEXT PASS]\n");
}
