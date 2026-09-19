#include "test.h"
#include <gtnh/net/server.h>
#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/tcp_connector.h>
#include <gtnh/net/frame.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>
#include <iostream>

static constexpr uint16_t kEchoPort = 19876;
static const char* kEchoPayload = "Hello from libgtnh-net echo test!";
static constexpr uint8_t kEchoType = 0x07;

void test_echo() {
    gtnh::net::TcpServer server;
    std::atomic<int> echo_count{0};

    std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> server_connections;
    std::mutex conn_mutex;

    server.on_accept = [&](int client_fd) {
        static gtnh::net::TagAllocator tag_alloc;
        auto tags = tag_alloc.alloc();
        auto conn = std::make_shared<gtnh::net::IoUringConnection>(
            client_fd, "echo-srv", tags);

        conn->on_message = [conn, &echo_count](uint8_t type, const uint8_t* data, size_t len) {
            conn->send(type, data, len);
            echo_count.fetch_add(1, std::memory_order_relaxed);
        };

        if (conn->start_reading()) {
            std::lock_guard<std::mutex> lock(conn_mutex);
            server_connections.push_back(std::move(conn));
        }
    };

    if (!server.listen(kEchoPort, "echo-server")) {
        printf("  [ECHO SKIP] port %d already in use\n", kEchoPort);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int fd = gtnh::net::TcpConnector::connect("127.0.0.1", kEchoPort, 5000);
    CHECK_GE(fd, 0);

    gtnh::net::TagAllocator tag_alloc;
    auto tags = tag_alloc.alloc();
    auto client_conn = std::make_unique<gtnh::net::IoUringConnection>(
        fd, "echo-cln", tags);

std::atomic<bool> echo_received{false};
std::optional<std::vector<uint8_t>> received_data;

client_conn->on_message = [&]([[maybe_unused]] uint8_t type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> buffer(data, data + len);
    received_data = std::move(buffer);
    echo_received.store(true, std::memory_order_release);
};

    CHECK(client_conn->start_reading());

    size_t payload_len = strlen(kEchoPayload);
    client_conn->send(kEchoType,
                       reinterpret_cast<const uint8_t*>(kEchoPayload),
                       payload_len);

    int waited = 0;
    while (!echo_received.load(std::memory_order_acquire) && waited < 500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++waited;
        if (waited % 50 == 0) {
            std::cerr << "  [DEBUG] waiting for echo, count=" << echo_count.load()
                      << " server_conns=" << server_connections.size()
                      << " client_conn_valid=" << (client_conn && client_conn->is_open()) << "\n";
        }
    }

CHECK(echo_received.load(), "echo not received within 5s timeout");
if (echo_received.load()) {
    CHECK(received_data.has_value(), "no data received");
    CHECK_EQ(received_data->size(), payload_len);
    CHECK(memcmp(received_data->data(), kEchoPayload, payload_len) == 0,
          "echo payload mismatch");
}

    client_conn->close();
    client_conn.reset();

    {
        std::lock_guard<std::mutex> lock(conn_mutex);
        for (auto& c : server_connections) {
            c->close();
        }
        server_connections.clear();
    }
    server.stop();

if (echo_received.load() && received_data.has_value()) {
    printf("  [ECHO PASS] sent %zu bytes, received %zu bytes, %d echoes\n",
           payload_len, received_data->size(), echo_count.load());
} else {
    printf("  [ECHO FAIL] no echo received\n");
}
}
