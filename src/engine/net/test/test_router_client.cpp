// ===========================================================================
//  RouterClient unit tests (beads gp-sst5)
//
//  Covers src/engine/net/src/router_client.cpp — the pub/sub client every
//  service uses to reach the MessageRouter. Since the Router commit barrier
//  landed (gp-onh), a reconnect or resubscription bug here shows up in the
//  integration harness as a service that never becomes "ready", so the wire
//  behaviour of connect / subscribe / reconnect is pinned here.
//
//  ── Test double ────────────────────────────────────────────────────────────
//  StubRouter is an in-process TCP peer speaking the router wire format
//  ([4B BE payload_len][1B type][payload], payload_len counts the type byte).
//  It records every frame the client sends, tagged with the accepted
//  connection index, and can be told to drop the connection or push a frame
//  back. The real Go router is never started; the listener is loopback on an
//  ephemeral port.
//
//  ── Determinism ────────────────────────────────────────────────────────────
//  RouterClient has NO timer-based retry: connect() is a single attempt and
//  the retry is the caller's job (see src/apps/gateway/main.cpp, the
//  "Router reconnect check" block). The header comment claiming
//  "Retries with exponential backoff 1s, 2s, 4s ... max 30s" describes
//  behaviour that is not in the .cpp, and connect_attempts_ is written once
//  and never read. So the retry path is driven EXPLICITLY here: disconnect(),
//  then connect() again — no sleeps, no backoff, no wall-clock racing.
//  Every wait in this file is a bounded fail-fast on a real event
//  (condvar signaled by the stub thread, or an atomic flag the client
//  itself sets). Nothing is "sleep and hope"; a timeout only decides how fast
//  a genuine failure is reported.
//
//  ── Harness ────────────────────────────────────────────────────────────────
//  The project has zero GTest usage (absent from conanfile.txt, not installed
//  in CI, and CI builds Release with global -Werror), so this file carries the
//  project's own CHECK/TEST harness, matching src/engine/net/test/test_main.cpp
//  and src/game/machines/test/test_explosion_system.cpp. It is a separate
//  executable, so the per-file definitions of test_check / g_failed do not
//  collide with gtnh_net_test at link time.
// ===========================================================================

#include <gtnh/net/router_client.h>

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
//  Project test harness (definitions near the top, before first use)
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
    if (msg)
      fprintf(stderr, " -- %s", msg);
    fprintf(stderr, "\n");
  }
}

#define CHECK(cond, ...)                                                       \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)

#define CHECK_EQ(a, b, ...)                                                    \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NE(a, b, ...)                                                    \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define CHECK_GE(a, b, ...)                                                    \
  test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)

#define TEST(name)                                                             \
  do {                                                                         \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

namespace {

using gtnh::net::RouterClient;
using gtnh::net::RouterMsg;

// Bound for every event wait in this file. Reaching it means a real bug, so it
// only controls how fast a failure is reported — it never drives behaviour.
constexpr int kWaitMs = 4000;

void note_finding(const char *text) { printf("    [FINDING] %s\n", text); }

uint16_t be16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                               static_cast<uint16_t>(p[1]));
}

// Build a raw router wire frame the way the Go router does.
std::vector<uint8_t> wire_frame(uint8_t type, const std::vector<uint8_t> &payload) {
  const uint32_t raw_len = 1u + static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> out(4 + raw_len);
  out[0] = static_cast<uint8_t>(raw_len >> 24);
  out[1] = static_cast<uint8_t>(raw_len >> 16);
  out[2] = static_cast<uint8_t>(raw_len >> 8);
  out[3] = static_cast<uint8_t>(raw_len);
  out[4] = type;
  if (!payload.empty())
    std::memcpy(out.data() + 5, payload.data(), payload.size());
  return out;
}

// ---------------------------------------------------------------------------
//  StubRouter — in-process stub server socket
// ---------------------------------------------------------------------------

struct StubFrame {
  int conn = -1;
  uint8_t type = 0;
  std::vector<uint8_t> payload;
};

class StubRouter {
public:
  explicit StubRouter(uint16_t port = 0) { open_listener(port); }

  ~StubRouter() { teardown(); }

  StubRouter(const StubRouter &) = delete;
  StubRouter &operator=(const StubRouter &) = delete;

  uint16_t port() const { return port_; }

  // -- observation ----------------------------------------------------------

  size_t frame_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return frames_.size();
  }

  size_t frames_on(int conn) const {
    std::lock_guard<std::mutex> lock(mu_);
    return frames_on_locked(conn);
  }

  std::vector<StubFrame> frames_for(int conn) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<StubFrame> out;
    for (const auto &f : frames_)
      if (f.conn == conn)
        out.push_back(f);
    return out;
  }

  int accept_count() const { return accepts_.load(std::memory_order_acquire); }

  std::vector<int> accepted_fds() const {
    std::lock_guard<std::mutex> lock(mu_);
    return accepted_fds_;
  }

  // Bounded, event-driven wait: returns true when the condition holds.
  bool wait_frames(int conn, size_t count) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (frames_on_locked(conn) < count) {
      if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        return frames_on_locked(conn) >= count;
    }
    return true;
  }

  bool wait_accepts(int count) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (accepts_.load(std::memory_order_acquire) < count) {
      if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        return accepts_.load(std::memory_order_acquire) >= count;
    }
    return true;
  }

  // -- actions --------------------------------------------------------------

  // Graceful FIN to the current client connection. The stub thread owns the
  // fd and performs the close once its blocking read() returns 0.
  void drop_client() {
    int fd = conn_.load(std::memory_order_acquire);
    if (fd >= 0)
      ::shutdown(fd, SHUT_RDWR);
  }

  bool send_to_client(uint8_t type, const std::vector<uint8_t> &payload) {
    int fd = conn_.load(std::memory_order_acquire);
    if (fd < 0)
      return false;
    const std::vector<uint8_t> buf = wire_frame(type, payload);
    size_t sent = 0;
    while (sent < buf.size()) {
      ssize_t n = ::send(fd, buf.data() + sent, buf.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        if (n < 0 && errno == EINTR)
          continue;
        return false;
      }
      sent += static_cast<size_t>(n);
    }
    return true;
  }

private:
  void open_listener(uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    if (listen_fd_ >= 0)
      ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0)
      pipe_fds[0] = pipe_fds[1] = -1;
    wake_read_ = pipe_fds[0];
    wake_write_ = pipe_fds[1];

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (listen_fd_ < 0 ||
        ::bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      fprintf(stderr, "  [STUB] listen failed: %s\n", std::strerror(errno));
      if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
      }
      return;
    }

    socklen_t alen = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr),
                      &alen) != 0)
      return;
    port_ = ntohs(addr.sin_port);

    thread_ = std::thread([this] { run(); });
  }

  void teardown() {
    stop_.store(true, std::memory_order_release);
    if (wake_write_ >= 0) {
      const char b = 'x';
      ssize_t ignored = ::write(wake_write_, &b, 1);
      (void)ignored;
    }
    if (thread_.joinable())
      thread_.join();
    int fd = conn_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0)
      ::close(fd);
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (wake_read_ >= 0) {
      ::close(wake_read_);
      wake_read_ = -1;
    }
    if (wake_write_ >= 0) {
      ::close(wake_write_);
      wake_write_ = -1;
    }
  }

  size_t frames_on_locked(int conn) const {
    size_t n = 0;
    for (const auto &f : frames_)
      if (f.conn == conn)
        ++n;
    return n;
  }

  void record(int conn, const uint8_t *p, size_t len) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      frames_.push_back(StubFrame{conn, p[0], std::vector<uint8_t>(p + 1, p + len)});
    }
    cv_.notify_all();
  }

  void handle_accept() {
    for (;;) {
      int fd = ::accept4(listen_fd_, nullptr, nullptr,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0)
        return;
      int idx = -1;
      {
        std::lock_guard<std::mutex> lock(mu_);
        accepted_fds_.push_back(fd);
        idx = static_cast<int>(accepted_fds_.size()) - 1;
      }
      int stale = conn_.exchange(fd, std::memory_order_acq_rel);
      if (stale >= 0)
        ::close(stale);
      // Do NOT index frames by fd: the kernel legally recycles the descriptor
      // number across close/connect, so a later connection can reuse the same
      // int. The accept ordinal is the stable identity.
      conn_index_.store(idx, std::memory_order_release);
      accepts_.store(idx + 1, std::memory_order_release);
      cv_.notify_all();
    }
  }

  void handle_read() {
    int fd = conn_.load(std::memory_order_acquire);
    if (fd < 0)
      return;

    std::vector<uint8_t> buf;
    for (;;) {
      uint8_t tmp[4096];
      ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        // EAGAIN: buffer drained. MUST return to poll() — spinning here
        // would never observe the teardown wakeup.
        break;
      }
      if (n == 0) { // peer closed
        int gone = conn_.exchange(-1, std::memory_order_acq_rel);
        if (gone >= 0)
          ::close(gone);
        return;
      }
      buf.insert(buf.end(), tmp, tmp + n);

      size_t pos = 0;
      const int idx = conn_index_.load(std::memory_order_acquire);
      while (buf.size() - pos >= 5) {
        const uint32_t raw =
            (static_cast<uint32_t>(buf[pos]) << 24) |
            (static_cast<uint32_t>(buf[pos + 1]) << 16) |
            (static_cast<uint32_t>(buf[pos + 2]) << 8) |
            static_cast<uint32_t>(buf[pos + 3]);
        if (raw == 0) {
          ++pos;
          continue;
        }
        const size_t total = 4 + static_cast<size_t>(raw);
        if (buf.size() - pos < total)
          break;
        record(idx, buf.data() + pos + 4, static_cast<size_t>(raw));
        pos += total;
      }
      if (pos > 0)
        buf.erase(buf.begin(), buf.begin() + static_cast<long>(pos));
    }
  }

  void run() {
    while (!stop_.load(std::memory_order_acquire)) {
      struct pollfd pfds[3];
      pfds[0].fd = listen_fd_;
      pfds[0].events = POLLIN;
      pfds[0].revents = 0;
      pfds[1].fd = conn_.load(std::memory_order_acquire);
      pfds[1].events = POLLIN;
      pfds[1].revents = 0;
      pfds[2].fd = wake_read_;
      pfds[2].events = POLLIN;
      pfds[2].revents = 0;

      int rc = ::poll(pfds, 3, -1); // blocked forever; woken only by events
      if (rc < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (pfds[2].revents & POLLIN)
        break; // teardown()
      if (pfds[0].revents & POLLIN)
        handle_accept();
      if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
        handle_read();
    }
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<StubFrame> frames_;
  std::vector<int> accepted_fds_;
  std::atomic<int> conn_{-1};
  std::atomic<int> conn_index_{-1};
  std::atomic<int> accepts_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
  int listen_fd_ = -1;
  int wake_read_ = -1;
  int wake_write_ = -1;
  uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
//  Wire decoders — decode what the client actually put on the wire
// ---------------------------------------------------------------------------

struct RegisterFrame {
  std::string service;
  std::vector<std::string> topics;
};

bool decode_register(const std::vector<uint8_t> &p, RegisterFrame &out) {
  size_t i = 0;
  if (p.size() < 2)
    return false;
  uint16_t n = be16(p.data() + i);
  i += 2;
  if (i + static_cast<size_t>(n) > p.size())
    return false;
  out.service.assign(reinterpret_cast<const char *>(p.data() + i), n);
  i += n;
  if (i + 2 > p.size())
    return false;
  uint16_t ntopics = be16(p.data() + i);
  i += 2;
  for (uint16_t k = 0; k < ntopics; ++k) {
    if (i + 2 > p.size())
      return false;
    uint16_t tl = be16(p.data() + i);
    i += 2;
    if (i + static_cast<size_t>(tl) > p.size())
      return false;
    out.topics.emplace_back(reinterpret_cast<const char *>(p.data() + i), tl);
    i += tl;
  }
  return i == p.size();
}

bool decode_subscribe(const std::vector<uint8_t> &p, std::string &out) {
  if (p.size() < 2)
    return false;
  const uint16_t tl = be16(p.data());
  if (2 + static_cast<size_t>(tl) != p.size())
    return false;
  out.assign(reinterpret_cast<const char *>(p.data() + 2), tl);
  return true;
}

struct PublishFrame {
  std::string topic;
  std::vector<uint8_t> body;
};

bool decode_publish(const std::vector<uint8_t> &p, PublishFrame &out) {
  if (p.size() < 2)
    return false;
  const uint16_t tl = be16(p.data());
  if (2 + static_cast<size_t>(tl) > p.size())
    return false;
  out.topic.assign(reinterpret_cast<const char *>(p.data() + 2), tl);
  out.body.assign(p.begin() + 2 + tl, p.end());
  return true;
}

std::vector<uint8_t> bytes_of(std::initializer_list<int> vals) {
  std::vector<uint8_t> out;
  for (int v : vals)
    out.push_back(static_cast<uint8_t>(v));
  return out;
}

// Bind a loopback listener, learn its port, then release it. Connecting to a
// port nobody listens on fails immediately with ECONNREFUSED, which is how the
// "connect fails" test gets a deterministic negative without a race.
uint16_t reserve_then_release_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return 0;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  uint16_t port = 0;
  socklen_t alen = sizeof(addr);
  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 &&
      ::getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &alen) == 0)
    port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

// Block until `pred` holds, or fail fast. This waits on an atomic the client
// itself flips — it is observation, not a scheduler.
template <typename Pred>
bool wait_for(Pred pred) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}

} // namespace

// ===========================================================================
//  Tests
// ===========================================================================

// 1. connect() registers with the service name and reports connected.
void test_connect_registers_with_service_name() {
  StubRouter stub;
  CHECK_GE(static_cast<int>(stub.port()), 1, "stub listener must be bound");

  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "simcore"));
  CHECK(client.is_connected());
  CHECK(stub.wait_accepts(1));
  CHECK(stub.wait_frames(0, 1), "no frame arrived on connection 0");

  auto frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{1}, "connect must send exactly one frame");
  if (frames.size() == 1) {
    CHECK_EQ(frames[0].type, uint8_t(RouterMsg::kRegister));
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg), "undecodable register frame");
    CHECK_EQ(reg.service, std::string("simcore"));
    CHECK_EQ(reg.topics.size(), size_t{0}, "no topics subscribed yet");
  }
  client.disconnect();
  CHECK(!client.is_connected());
}

// 2. subscribe() before connect() is deferred: it puts nothing on the wire,
//    and connect() folds the topics into the Register frame.
void test_subscribe_before_connect_is_deferred_into_register() {
  StubRouter stub;
  RouterClient client;

  client.subscribe("block.changed");
  client.subscribe("player.joined");
  client.subscribe("block.changed"); // duplicate

  // Nothing may be sent before the connection exists. Proved negatively: after
  // connect() the first frame on the wire is the Register, not a Subscribe.
  CHECK(client.connect("127.0.0.1", stub.port(), "chunkstore"));
  CHECK(stub.wait_frames(0, 1));

  auto frames = stub.frames_for(0);
  CHECK_GE(frames.size(), size_t{1});
  if (!frames.empty()) {
    CHECK_EQ(frames[0].type, uint8_t(RouterMsg::kRegister),
             "first frame must be Register, not a deferred Subscribe");
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg));
    CHECK_EQ(reg.service, std::string("chunkstore"));
    CHECK_EQ(reg.topics.size(), size_t{2}, "duplicates must collapse");
    if (reg.topics.size() == 2) {
      CHECK_EQ(reg.topics[0], std::string("block.changed"),
               "insertion order must be preserved");
      CHECK_EQ(reg.topics[1], std::string("player.joined"));
    }
  }
  // A deferred subscribe never emits its own Subscribe frame; the router
  // learns the topics from Register. Send a marker to prove the socket is idle
  // before it, so the absence of a Subscribe frame is event-verified.
  client.subscribe("marker.after.connect");
  CHECK(stub.wait_frames(0, 2));
  frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{2}, "only Register + one post-connect Subscribe");
  if (frames.size() == 2) {
    CHECK_EQ(frames[1].type, uint8_t(RouterMsg::kSubscribe));
    std::string topic;
    CHECK(decode_subscribe(frames[1].payload, topic));
    CHECK_EQ(topic, std::string("marker.after.connect"));
  }
  client.disconnect();
}

// 3. subscribe() after connect() sends a Subscribe frame on the wire.
void test_subscribe_after_connect_sends_subscribe_frame() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "gateway"));
  CHECK(stub.wait_frames(0, 1));

  client.subscribe("gateway.client.join");
  client.subscribe("gateway.bulk.chunk");
  CHECK(stub.wait_frames(0, 3));

  auto frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{3});
  if (frames.size() == 3) {
    CHECK_EQ(frames[1].type, uint8_t(RouterMsg::kSubscribe));
    CHECK_EQ(frames[2].type, uint8_t(RouterMsg::kSubscribe));
    std::string t1, t2;
    CHECK(decode_subscribe(frames[1].payload, t1));
    CHECK(decode_subscribe(frames[2].payload, t2));
    CHECK_EQ(t1, std::string("gateway.client.join"));
    CHECK_EQ(t2, std::string("gateway.bulk.chunk"));
  }
  client.disconnect();
}

// 4. Duplicate subscribe() still transmits a frame but is registered once.
//    The Go router deduplicates on Register, so resubscription stays bounded.
void test_duplicate_subscribe_sends_frame_but_registers_once() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "entity_state_store"));
  CHECK(stub.wait_frames(0, 1));

  client.subscribe("entity.state.set");
  client.subscribe("entity.state.set");
  client.subscribe("entity.state.get");
  CHECK(stub.wait_frames(0, 4));
  CHECK_EQ(stub.frames_for(0).size(), size_t{4}, "3 Subscribe + 1 Register");

  client.disconnect();
  CHECK(client.connect("127.0.0.1", stub.port(), "entity_state_store"));
  CHECK(stub.wait_accepts(2));
  CHECK(stub.wait_frames(1, 1));

  auto frames = stub.frames_for(1);
  if (!frames.empty()) {
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg));
    CHECK_EQ(reg.topics.size(), size_t{2}, "duplicate must not be re-registered");
  }
  client.disconnect();
}

// 5. publish() encodes topic length (BE16) + topic + body.
void test_publish_encodes_topic_and_payload() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "meta_db"));
  CHECK(stub.wait_frames(0, 1));

  const std::vector<uint8_t> body = bytes_of({0xDE, 0xAD, 0xBE, 0xEF});
  client.publish("player.save", body);
  CHECK(stub.wait_frames(0, 2));

  auto frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{2});
  if (frames.size() == 2) {
    CHECK_EQ(frames[1].type, uint8_t(RouterMsg::kPublish));
    PublishFrame pub;
    CHECK(decode_publish(frames[1].payload, pub));
    CHECK_EQ(pub.topic, std::string("player.save"));
    CHECK_EQ(pub.body.size(), size_t{4});
    CHECK_EQ(pub.body[0], uint8_t{0xDE});
    CHECK_EQ(pub.body[3], uint8_t{0xEF});
  }

  // Empty body is still a valid publish.
  client.publish("player.save", std::vector<uint8_t>{});
  CHECK(stub.wait_frames(0, 3));
  frames = stub.frames_for(0);
  if (frames.size() == 3) {
    PublishFrame pub;
    CHECK(decode_publish(frames[2].payload, pub));
    CHECK_EQ(pub.topic, std::string("player.save"));
    CHECK_EQ(pub.body.size(), size_t{0});
  }
  client.disconnect();
}

// 6. THE reconnect/resubscription test. disconnect() is synchronous; the retry
//    is driven by an explicit second connect() — no timer, no sleep.
void test_disconnect_then_connect_restores_registration() {
  StubRouter stub;
  RouterClient client;

  CHECK(client.connect("127.0.0.1", stub.port(), "pipe_network"));
  CHECK(stub.wait_accepts(1));
  CHECK(stub.wait_frames(0, 1));

  client.subscribe("pipe.network.updated");
  client.subscribe("pipe.tick");
  CHECK(stub.wait_frames(0, 3), "subscriptions must reach the wire");
  CHECK_EQ(stub.frames_for(0).size(), size_t{3});

  // Explicit disconnect — returns only once the connection is torn down, so
  // every assertion below is race-free.
  client.disconnect();
  CHECK(!client.is_connected());
  CHECK(client.connection() == nullptr, "disconnect must release the connection");

  // Explicit retry. This is the seam the services use (gateway/main.cpp).
  CHECK(client.connect("127.0.0.1", stub.port(), "pipe_network"));
  CHECK(stub.wait_accepts(2), "second connect must reach the stub");
  CHECK(stub.wait_frames(1, 1), "reconnect must re-register");
  CHECK(client.is_connected());

  // NB: the kernel recycles descriptor numbers, so a new connection can reuse
  // the previous fd int. The stable identity is the accept ordinal, already
  // asserted via wait_accepts(2). What must hold is that the old connection is
  // abandoned: no further bytes land on it after the reconnect.
  CHECK_EQ(stub.accept_count(), 2, "exactly one reconnect, no connect storm");

  auto frames = stub.frames_for(1);
  CHECK_EQ(frames.size(), size_t{1}, "reconnect re-registers, does not replay");
  if (!frames.empty()) {
    CHECK_EQ(frames[0].type, uint8_t(RouterMsg::kRegister));
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg));
    CHECK_EQ(reg.service, std::string("pipe_network"));
    CHECK_EQ(reg.topics.size(), size_t{2}, "BOTH topics must be resubscribed");
    if (reg.topics.size() == 2) {
      CHECK_EQ(reg.topics[0], std::string("pipe.network.updated"));
      CHECK_EQ(reg.topics[1], std::string("pipe.tick"));
    }
  }

  // The restored client is fully live again: the publish issued after the
  // reconnect is the second frame on connection 1, and nothing was written to
  // the abandoned connection 0.
  const size_t conn0_after = stub.frames_for(0).size();
  client.publish("pipe.tick", bytes_of({0x7}));
  CHECK(stub.wait_frames(1, 2), "post-reconnect publish must reach the wire");
  CHECK_EQ(stub.frames_for(0).size(), conn0_after,
           "no bytes may land on the abandoned connection");
  auto after = stub.frames_for(1);
  CHECK_EQ(after.size(), size_t{2}, "Register + Publish, nothing replayed");
  if (after.size() == 2) {
    CHECK_EQ(after[1].type, uint8_t(RouterMsg::kPublish));
    PublishFrame pub;
    CHECK(decode_publish(after[1].payload, pub));
    CHECK_EQ(pub.topic, std::string("pipe.tick"));
  }
  client.disconnect();
}

// 7. Peer-side close is detected by the client, and the client recovers on an
//    explicit reconnect.
//
//    WAIT TARGET MATTERS. IoUringConnection::cleanup() does fd_.exchange(-1)
//    and only THEN fires on_closed, and on_closed is what clears
//    RouterClient::connected_. So is_open() goes false strictly before
//    connected_ does. Waiting on is_open() and then reading is_connected()
//    races that window; this waits on is_connected() — the semantic condition —
//    which by construction is set last.
void test_peer_close_marks_client_disconnected() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "recipe_manager"));
  CHECK(stub.wait_accepts(1));
  CHECK(stub.wait_frames(0, 1));
  CHECK(client.is_connected());

  stub.drop_client(); // FIN from the stub side
  CHECK(wait_for([&] { return !client.is_connected(); }),
        "client must notice the peer close and clear connected_");
  // on_closed has now run, so the fd teardown that precedes it is visible.
  CHECK(!client.connection()->is_open(), "the fd must already be closed");

  // And the client reconnects cleanly over the same stub.
  CHECK(client.connect("127.0.0.1", stub.port(), "recipe_manager"),
        "explicit reconnect after a peer close must succeed");
  CHECK(stub.wait_accepts(2));
  CHECK(stub.wait_frames(1, 1));
  CHECK(client.is_connected());
  auto frames = stub.frames_for(1);
  CHECK_EQ(frames.size(), size_t{1}, "reconnect after peer close re-registers");
  if (!frames.empty())
    CHECK_EQ(frames[0].type, uint8_t(RouterMsg::kRegister));
  client.disconnect();

  note_finding("RouterClient::connected_ and registered_ are plain bools "
               "written from the IoUringConnection poll thread (on_closed) and "
               "read from the main thread by is_connected(). That is a "
               "cross-thread data race on a non-atomic; the observed order is "
               "cleanup() clearing fd_ before on_closed fires.");
}

// 8. While disconnected, publish() is a SILENT DROP and a deferred subscribe()
//    is only remembered. Neither is reported to the caller: both return void.
//    The drop is proven event-wise by observing that nothing extra appears on
//    connection 0 while connection 1 receives only the expected frames.
void test_publish_and_subscribe_while_disconnected_are_dropped() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "world_generator"));
  CHECK(stub.wait_accepts(1));
  CHECK(stub.wait_frames(0, 1));

  client.disconnect();
  CHECK(!client.is_connected());

  client.publish("world.chunk.request", bytes_of({1, 2, 3}));
  client.subscribe("world.chunk.ready"); // deferred
  client.heartbeat();
  client.health_request(0x0102030405060708ULL);

  // Explicit retry; a topic subscribed while down is restored via Register.
  CHECK(client.connect("127.0.0.1", stub.port(), "world_generator"));
  CHECK(stub.wait_accepts(2));
  CHECK(stub.wait_frames(1, 1));

  CHECK_EQ(stub.frames_for(0).size(), size_t{1},
           "down-period publish/heartbeat/health_request must not be queued");
  note_finding("publish(), heartbeat() and health_request() return void and "
               "early-return when !connected_ — a send failure cannot surface "
               "as an error, it is a silent drop. Same for a deferred "
               "subscribe(): it is remembered, not sent, and not reported.");

  auto frames = stub.frames_for(1);
  CHECK_EQ(frames.size(), size_t{1});
  if (!frames.empty()) {
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg));
    CHECK_EQ(reg.topics.size(), size_t{1});
    if (reg.topics.size() == 1)
      CHECK_EQ(reg.topics[0], std::string("world.chunk.ready"),
               "topic subscribed while down must be restored on reconnect");
  }
  client.disconnect();
}

// 9. The retry path itself: a refused connect returns false and leaves the
//    client disconnected; retrying explicitly on the same port then succeeds.
void test_reconnect_retries_explicitly_after_refused_connect() {
  const uint16_t port = reserve_then_release_port();
  CHECK_GE(static_cast<int>(port), 1);

  RouterClient client;
  CHECK(!client.connect("127.0.0.1", port, "simcore"),
        "connect to a dead port must fail");
  CHECK(!client.is_connected(), "a failed connect must not report connected");
  CHECK(client.connection() == nullptr);

  // A topic taken while down must survive the failed connect.
  client.subscribe("quest.book.open");

  StubRouter stub(port); // same port, now actually listening
  CHECK_GE(static_cast<int>(stub.port()), 1);
  CHECK(client.connect("127.0.0.1", stub.port(), "simcore"),
        "explicit retry after a refused connect must succeed");
  CHECK(stub.wait_accepts(1));
  CHECK(stub.wait_frames(0, 1));
  CHECK(client.is_connected());

  auto frames = stub.frames_for(0);
  if (!frames.empty()) {
    RegisterFrame reg;
    CHECK(decode_register(frames[0].payload, reg));
    CHECK_EQ(reg.service, std::string("simcore"));
    CHECK_EQ(reg.topics.size(), size_t{1},
             "topic taken before the first successful connect must be sent");
    if (reg.topics.size() == 1)
      CHECK_EQ(reg.topics[0], std::string("quest.book.open"));
  }
  note_finding("RouterClient has no built-in retry: connect() is one attempt, "
               "connect_attempts_ is written but never read, and the header's "
               "'Retries with exponential backoff 1s, 2s, 4s ... max 30s' is "
               "not implemented. Every service retries by hand "
               "(e.g. gateway/main.cpp 'Router reconnect check').");
  client.disconnect();
}

// 10. connect() without a service name succeeds but never registers. The
//     service would look connected and stay invisible to the router.
void test_connect_without_service_name_never_registers() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), ""));
  CHECK(client.is_connected(), "connect still reports success");

  // Marker frame proves the connection is alive and no Register preceded it.
  client.subscribe("marker");
  CHECK(stub.wait_frames(0, 1));

  auto frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{1}, "no register frame may be sent");
  if (frames.size() == 1)
    CHECK_EQ(frames[0].type, uint8_t(RouterMsg::kSubscribe),
             "the only frame must be the marker Subscribe");
  note_finding("connect(host, port, \"\") returns true with no register frame "
               "and no error signal; registered_ stays false. Nothing in the "
               "readiness path can see this.");
  client.disconnect();
}

// 11. An incoming publish frame is dispatched to on_publish.
void test_incoming_publish_reaches_callback() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "in-game"));
  // The stub sends on the accepted fd, so the accept must have happened first.
  // (Sending before accept() is what made this test flaky.)
  CHECK(stub.wait_accepts(1), "stub must accept before it can send");

  auto got = std::make_shared<std::atomic<int>>(0);
  auto topic = std::make_shared<std::string>();
  auto body = std::make_shared<std::vector<uint8_t>>();
  auto arrived = std::make_shared<std::atomic<bool>>(false);
  client.on_publish = [got, topic, body, arrived](
                          const std::string &t,
                          std::shared_ptr<std::vector<uint8_t>> d) {
    *topic = t;
    *body = *d;
    got->fetch_add(1, std::memory_order_acq_rel);
    arrived->store(true, std::memory_order_release);
  };

  std::vector<uint8_t> payload;
  payload.push_back(0x00);
  payload.push_back(0x0B); // topic_len = 11
  const std::string t = "block.event";
  payload.insert(payload.end(), t.begin(), t.end());
  const std::vector<uint8_t> b = bytes_of({0x7A, 0x7B, 0x7C});
  payload.insert(payload.end(), b.begin(), b.end());
  CHECK(stub.send_to_client(uint8_t(RouterMsg::kPublish), payload));

  CHECK(wait_for([&] { return arrived->load(std::memory_order_acquire); }),
        "incoming publish never reached on_publish");
  CHECK_EQ(got->load(), 1);
  if (arrived->load(std::memory_order_acquire)) {
    CHECK_EQ(*topic, t);
    CHECK_EQ(body->size(), size_t{3});
    if (body->size() == 3) {
      CHECK_EQ((*body)[0], uint8_t{0x7A});
      CHECK_EQ((*body)[2], uint8_t{0x7C});
    }
  }
  client.disconnect();
}

// 12. Malformed publish frames are dropped, the stream stays in sync, and the
//     connection survives. Verified event-wise: after two bad frames a good
//     one still arrives, and only one callback fired.
void test_malformed_publish_frames_are_rejected() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "fuzzer"));
  CHECK(stub.wait_accepts(1), "stub must accept before it can send");

  auto got = std::make_shared<std::atomic<int>>(0);
  auto topic = std::make_shared<std::string>();
  client.on_publish = [got, topic](const std::string &t,
                                   std::shared_ptr<std::vector<uint8_t>>) {
    *topic = t;
    got->fetch_add(1, std::memory_order_acq_rel);
  };

  // len < 2: no room for the topic length
  CHECK(stub.send_to_client(uint8_t(RouterMsg::kPublish), bytes_of({0x01})));
  // topic_len = 5 but only 1 byte follows
  CHECK(stub.send_to_client(uint8_t(RouterMsg::kPublish),
                            bytes_of({0x00, 0x05, 'a'})));

  std::vector<uint8_t> good;
  good.push_back(0x00);
  good.push_back(0x02);
  good.push_back('o');
  good.push_back('k');
  CHECK(stub.send_to_client(uint8_t(RouterMsg::kPublish), good));

  CHECK(wait_for([&] { return got->load(std::memory_order_acquire) > 0; }),
        "the valid frame must still be delivered after two bad ones");
  CHECK_EQ(got->load(), 1, "both malformed frames must be dropped silently");
  CHECK_EQ(*topic, std::string("ok"));
  CHECK(client.is_connected(), "a bad frame must not kill the connection");
  CHECK(client.connection()->is_open());

  // A heartbeat after the garbage still gets out — write path is unaffected.
  client.heartbeat();
  CHECK(stub.wait_frames(0, 2));
  client.disconnect();
}

// 13. Heartbeat and health frames on the wire, including the BE nonce.
void test_heartbeat_and_health_request_frames() {
  StubRouter stub;
  RouterClient client;
  CHECK(client.connect("127.0.0.1", stub.port(), "probe"));
  CHECK(stub.wait_frames(0, 1));

  client.heartbeat();
  client.health_request(0x0102030405060708ULL);
  client.health_request(0);
  CHECK(stub.wait_frames(0, 4));

  auto frames = stub.frames_for(0);
  CHECK_EQ(frames.size(), size_t{4});
  if (frames.size() == 4) {
    CHECK_EQ(frames[1].type, uint8_t(RouterMsg::kHeartbeat));
    CHECK_EQ(frames[1].payload.size(), size_t{0}, "heartbeat carries no payload");
    CHECK_EQ(frames[2].type, uint8_t(RouterMsg::kHealthRequest));
    CHECK_EQ(frames[2].payload.size(), size_t{sizeof(uint64_t)});
    const std::vector<uint8_t> want = {0x01, 0x02, 0x03, 0x04,
                                       0x05, 0x06, 0x07, 0x08};
    CHECK(frames[2].payload == want, "nonce must be big-endian");
    CHECK_EQ(frames[3].type, uint8_t(RouterMsg::kHealthRequest));
  }
  client.disconnect();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {
  // RouterClient logs at info on every connect/subscribe; keep the test output
  // readable without hiding warnings and errors.
  spdlog::set_level(spdlog::level::warn);

  printf("=== RouterClient unit tests (gp-sst5) ===\n\n");

  TEST(connect_registers_with_service_name);
  TEST(subscribe_before_connect_is_deferred_into_register);
  TEST(subscribe_after_connect_sends_subscribe_frame);
  TEST(duplicate_subscribe_sends_frame_but_registers_once);
  TEST(publish_encodes_topic_and_payload);
  TEST(disconnect_then_connect_restores_registration);
  TEST(peer_close_marks_client_disconnected);
  TEST(publish_and_subscribe_while_disconnected_are_dropped);
  TEST(reconnect_retries_explicitly_after_refused_connect);
  TEST(connect_without_service_name_never_registers);
  TEST(incoming_publish_reaches_callback);
  TEST(malformed_publish_frames_are_rejected);
  TEST(heartbeat_and_health_request_frames);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
