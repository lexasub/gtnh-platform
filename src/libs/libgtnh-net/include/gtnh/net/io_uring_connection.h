#pragma once

#include "types.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gtnh::net {

// Read phase state machine
enum class ReadPhase : uint8_t { HEADER, PAYLOAD };

// In-flight write with partial write offset tracking
struct WriteOp {
  std::shared_ptr<std::vector<uint8_t>> frame;
  size_t offset = 0;
};

// Tags for one connection instance, allocated from TagAllocator.
struct ConnectionTags {
  uint64_t hdr_tag;    // read header completion
  uint64_t pay_tag;    // read payload completion
  uint64_t write_base; // base for write tags (write_base + seq)
};

// Global tag allocator — ensures tags are unique across all connections.
// Each alloc() returns a non-overlapping range of 1000 tags.
class TagAllocator {
public:
  ConnectionTags alloc() {
    uint64_t base = next_.fetch_add(1000, std::memory_order_relaxed);
    return {base, base + 1, base + 2};
  }

private:
  std::atomic<uint64_t> next_{1};
};

// Thread-safe io_uring TCP connection.
//
// Wire protocol: [4 bytes: payload length BE][1 byte: message type][payload]
//
// Each connection instance gets a unique generation; stale CQEs from a
// previous instance using the same tags are rejected. Tags are encoded in
// user_data as: (generation_ << kTagBits) | raw_tag.
class IoUringConnection {
public:
  IoUringConnection(int fd, const char *name, ConnectionTags tags);
  ~IoUringConnection();

  IoUringConnection(const IoUringConnection &) = delete;
  IoUringConnection &operator=(const IoUringConnection &) = delete;

  // Start the read loop: submit first header read.
  bool start_reading();

  // Thread-safe: build wire frame [4B BE len][1B type][payload] and enqueue.
  void send(uint8_t type, const uint8_t *data, size_t len);

  // Thread-safe: enqueue pre-built raw frame.
  void send_raw(std::shared_ptr<std::vector<uint8_t>> frame);

  // CQE dispatch — called from connection's own poll loop.
  // Returns true if this CQE belongs to this connection.
  bool on_cqe(int res, uint64_t user_data);

  // Close connection, drain writes, fire on_closed.
  void close();
  bool init_write_ring(unsigned entries);
  bool init_read_ring_internal(unsigned entries);
  void exit_rings();
  void poll_loop(std::shared_ptr<std::promise<bool>> read_ready);

  bool is_open() const { return fd_.load(std::memory_order_acquire) >= 0; }
  int fd() const { return fd_.load(std::memory_order_acquire); }

  // Fired when a complete message (header + payload) arrives.
  std::move_only_function<void(uint8_t type, const uint8_t *data, size_t len)>
      on_message;

  // Fired after close completes (fd closed, write queue drained).
  std::move_only_function<void()> on_closed;

private:
  bool prep_read_header();
  bool prep_read_payload();
  void on_read_header_complete(int res);
  void on_read_payload_complete(int res);
  void on_write_complete(int res, uint64_t user_data);
  void start_next_writes_locked();
  void start_next_writes();
  void cleanup();

  std::atomic<int> fd_{-1};
  std::string name_;

  // Generational tagging — incremented on each close() so stale CQEs
  // from a previous life don't match the new connection.
  uint64_t generation_ = 0;
  static std::atomic<uint64_t> next_generation_;

  // Tags
  uint64_t tag_hdr_;
  uint64_t tag_pay_;
  uint64_t tag_write_;

  // Read state
  ReadPhase read_phase_ = ReadPhase::HEADER;
  uint8_t header_[5] = {};
  size_t header_got_ = 0;
  unsigned consecutive_bad_headers_ = 0;
  size_t payload_got_ = 0;
  uint32_t expected_payload_ = 0;
  std::vector<uint8_t> payload_buf_;
  std::chrono::steady_clock::time_point connect_start_ =
      std::chrono::steady_clock::now();
  bool grace_elapsed() const {
    return std::chrono::steady_clock::now() - connect_start_ >
           std::chrono::milliseconds(2000);
  }

  // Write queue
  std::mutex write_mutex_;
  std::deque<std::unique_ptr<WriteOp>> write_queue_;
  std::unordered_map<uint64_t, std::unique_ptr<WriteOp>> in_flight_writes_;
  uint64_t next_write_seq_ = 0;

  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> close_pending_{false};

  io_uring ring_{};
  io_uring ring_write_{};
  std::thread poll_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> read_ring_inited_{false};
  std::atomic<bool> write_ring_inited_{false};
  std::mutex sq_mutex_;
  std::mutex sq_mutex_write_;
};

} // namespace gtnh::net
