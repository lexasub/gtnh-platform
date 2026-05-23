#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/server.h>
#include <gtnh/net/frame.h>
#include <gtnh/net/tcp_connector.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

static constexpr int kNumFrames   = 10;
static constexpr int kPayloadSize = 229432;
static constexpr auto kInterFrameDelay = std::chrono::milliseconds(20);

static std::atomic<int> g_frames_sent{0};
static std::atomic<int> g_frames_ok{0};
static std::atomic<int> g_bad_frames{0};
static std::atomic<bool> g_done{false};

static void write_seq_num(uint8_t* buf, uint64_t seq) {
    for (int i = 0; i < 8; ++i)
        buf[i] = static_cast<uint8_t>((seq >> (56 - 8 * i)) & 0xFF);
}

static uint64_t read_seq_num(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | buf[i];
    return v;
}

int main(int argc, char** argv) {
    uint16_t port = 9877;
    const char* host = "127.0.0.1";

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::atoi(argv[2]));

    fprintf(stderr, "=== IoUringConnection integration test ===\n");
    fprintf(stderr, "Server: 127.0.0.1:%u\n", port);
    fprintf(stderr, "Client: %s:%u\n", host, port);
    fprintf(stderr, "Frames: %d x %d bytes\n\n", kNumFrames, kPayloadSize);

    // Keep connections alive for the test duration
    std::unique_ptr<gtnh::net::IoUringConnection> server_conn;
    std::unique_ptr<gtnh::net::IoUringConnection> client_conn;

    // -- Server setup --
    gtnh::net::TcpServer tcp_server;
    tcp_server.on_accept = [&](int client_fd) {
        gtnh::net::TagAllocator alloc;
        auto tags = alloc.alloc();
        server_conn = std::make_unique<gtnh::net::IoUringConnection>(
            client_fd, "test-server", tags);

        server_conn->on_message = [](uint8_t, const uint8_t*, size_t) {};
        server_conn->on_closed  = []() { g_done = true; };

        if (!server_conn->start_reading()) {
            fprintf(stderr, "SERVER: start_reading failed\n");
            exit(1);
        }

        std::vector<uint8_t> payload(kPayloadSize);
        for (int i = 0; i < kPayloadSize; ++i)
            payload[i] = static_cast<uint8_t>(i & 0xFF);

        for (int f = 0; f < kNumFrames; ++f) {
            write_seq_num(payload.data(), static_cast<uint64_t>(f));
            server_conn->send(static_cast<uint8_t>(f & 0xFF),
                              payload.data(), payload.size());
            ++g_frames_sent;
            std::this_thread::sleep_for(kInterFrameDelay);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
        server_conn->close();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    };

    if (!tcp_server.listen(port, "test-srv")) {
        fprintf(stderr, "SERVER: listen failed on port %u\n", port);
        return 1;
    }
    fprintf(stderr, "SERVER: listening on port %u\n", port);

    // -- Client connect (retry until server accepts) --
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int fd = -1;
    while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
        fd = gtnh::net::TcpConnector::connect(host, port);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (fd < 0) {
        fprintf(stderr, "CLIENT: connect to %s:%u failed\n", host, port);
        return 1;
    }

    {
        gtnh::net::TagAllocator alloc;
        auto tags = alloc.alloc();
        client_conn = std::make_unique<gtnh::net::IoUringConnection>(
            fd, "test-client", tags);

        client_conn->on_message = []([[maybe_unused]] uint8_t msg_type, const uint8_t* data, size_t len) {
            if (len != kPayloadSize) {
                fprintf(stderr, "CLIENT: bad payload size: expected=%d got=%zu\n",
                        kPayloadSize, len);
                ++g_bad_frames;
                return;
            }
            uint64_t seq = read_seq_num(data);
            bool ok = true;
            for (size_t i = 8; i < len; ++i) {
                if (data[i] != static_cast<uint8_t>(i & 0xFF)) {
                    if (ok) {
                        fprintf(stderr, "CLIENT: frame seq=%lu payload corruption at byte %zu\n",
                                (unsigned long)seq, i);
                        ok = false;
                    }
                    ++g_bad_frames;
                    break;
                }
            }
            if (ok) {
                ++g_frames_ok;
                fprintf(stderr, "CLIENT: frame seq=%lu OK (%d/%d)\n",
                        (unsigned long)seq, g_frames_ok.load(), kNumFrames);
            }
        };
        client_conn->on_closed = []() { g_done = true; };

        if (!client_conn->start_reading()) {
            fprintf(stderr, "CLIENT: start_reading failed\n");
            return 1;
        }
    }

    // -- Wait for test to complete --
    std::this_thread::sleep_for(std::chrono::seconds(15));

    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "  Frames sent:       %d\n", g_frames_sent.load());
    fprintf(stderr, "  Frames received OK: %d\n", g_frames_ok.load());
    fprintf(stderr, "  Bad frames:        %d\n", g_bad_frames.load());

    bool pass = g_frames_ok.load() == kNumFrames && g_bad_frames.load() == 0;
    fprintf(stderr, "  %s\n\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
