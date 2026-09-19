#include "test.h"
#include <gtnh/net/frame.h>
#include <gtnh/net/types.h>
#include <cstdio>

static void test_write_be32() {
    uint8_t buf[4] = {};
    gtnh::net::frame::write_be32(buf, 0x01020304);
    CHECK_EQ(buf[0], 0x01);
    CHECK_EQ(buf[1], 0x02);
    CHECK_EQ(buf[2], 0x03);
    CHECK_EQ(buf[3], 0x04);

    gtnh::net::frame::write_be32(buf, 0);
    CHECK_EQ(buf[0], 0x00);
    CHECK_EQ(buf[1], 0x00);
    CHECK_EQ(buf[2], 0x00);
    CHECK_EQ(buf[3], 0x00);

    gtnh::net::frame::write_be32(buf, 0xFFFFFFFF);
    CHECK_EQ(buf[0], 0xFF);
    CHECK_EQ(buf[1], 0xFF);
    CHECK_EQ(buf[2], 0xFF);
    CHECK_EQ(buf[3], 0xFF);
}

static void test_read_be32() {
    uint8_t buf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK_EQ(gtnh::net::frame::read_be32(buf), 0xDEADBEEF);

    uint8_t zero[4] = {};
    CHECK_EQ(gtnh::net::frame::read_be32(zero), 0u);

    uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(gtnh::net::frame::read_be32(ones), 0xFFFFFFFFu);
}

static void test_pack() {
    uint8_t payload[] = {0x41, 0x42, 0x43};
    auto frame = gtnh::net::frame::pack(0x05, payload, 3);

    CHECK_EQ(frame->size(), 4 + 1 + 3); // header(4) + type(1) + payload(3)
    CHECK_EQ(gtnh::net::frame::read_be32(frame->data()), 4u); // len = 1 + 3 = 4
    CHECK_EQ((*frame)[4], 0x05);
    CHECK_EQ((*frame)[5], 0x41);
    CHECK_EQ((*frame)[6], 0x42);
    CHECK_EQ((*frame)[7], 0x43);
}

static void test_pack_empty() {
    auto frame = gtnh::net::frame::pack(0x00, nullptr, 0);
    CHECK_EQ(frame->size(), 5); // header(4) + type(1), no payload
    CHECK_EQ(gtnh::net::frame::read_be32(frame->data()), 1u); // len = 1 + 0 = 1
    CHECK_EQ((*frame)[4], 0x00);
}

static void test_pack_router() {
    uint8_t payload[] = {0xAA, 0xBB};
    auto frame = gtnh::net::frame::pack_router(0x04, payload, 2);

    CHECK_EQ(frame->size(), 4 + 1 + 2);
    CHECK_EQ(gtnh::net::frame::read_be32(frame->data()), 3u); // len = 1 + 2 = 3
    CHECK_EQ((*frame)[4], 0x04);
    CHECK_EQ((*frame)[5], 0xAA);
    CHECK_EQ((*frame)[6], 0xBB);
}

static void test_pack_router_heartbeat() {
    auto frame = gtnh::net::frame::pack_router(0x05, nullptr, 0);
    CHECK_EQ(frame->size(), 5);
    CHECK_EQ(gtnh::net::frame::read_be32(frame->data()), 1u);
    CHECK_EQ((*frame)[4], 0x05);
}

static void test_write_be16() {
    uint8_t buf[2] = {};
    gtnh::net::frame::write_be16(buf, 0x0102);
    CHECK_EQ(buf[0], 0x01);
    CHECK_EQ(buf[1], 0x02);
}

void test_frame() {
    printf("  - write_be32\n"); test_write_be32();
    printf("  - read_be32\n");  test_read_be32();
    printf("  - write_be16\n"); test_write_be16();
    printf("  - pack\n");       test_pack();
    printf("  - pack_empty\n"); test_pack_empty();
    printf("  - pack_router\n"); test_pack_router();
    printf("  - pack_router_heartbeat\n"); test_pack_router_heartbeat();
    printf("  [FRAME PASS]\n");
}
