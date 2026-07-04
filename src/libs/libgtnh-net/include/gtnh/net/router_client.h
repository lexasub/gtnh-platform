#pragma once

#include "io_uring_connection.h"
#include "tcp_connector.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gtnh::net {

// Router protocol message types — must match Go message_router/router.go
// MsgType
enum class RouterMsg : uint8_t {
  kSubscribe = 0x01,
  kUnsubscribe = 0x02,
  kPublish = 0x03,
  kRegister = 0x04,
  kHeartbeat = 0x05,
};

// High-level Router pub/sub client.
//
// Each instance owns its own IoUringContext — full isolation from other
// connections. Connect, register with a service name, subscribe to topics,
// publish messages, send heartbeats. Reconnect with exponential backoff.
class RouterClient {
public:
  RouterClient() = default;
  ~RouterClient() { disconnect(); }

  RouterClient(const RouterClient &) = delete;
  RouterClient &operator=(const RouterClient &) = delete;
  RouterClient(RouterClient &&) = delete;
  RouterClient &operator=(RouterClient &&) = delete;

  // Connect to router and register as service_name.
  // Retries with exponential backoff 1s, 2s, 4s ... max 30s.
  // Returns true on success.
  bool connect(const char *host, uint16_t port, const char *service_name);

  // Disconnect and shutdown context.
  void disconnect();

  bool is_connected() const { return connected_; }

  // Subscribe to a topic. May be called before connect (deferred).
  void subscribe(const std::string &topic);

  // Publish a message on a topic.
  void publish(const std::string &topic, const uint8_t *data, size_t len);
  void publish(const std::string &topic, const std::vector<uint8_t> &data);

  // Send heartbeat (must be called periodically from main loop).
  void heartbeat();

  // Callback for incoming publish messages.
  // Called from IoUringContext poll thread — do not block.
  std::move_only_function<void(const std::string &topic,
                               std::shared_ptr<std::vector<uint8_t>> data)>
      on_publish;

  // Access underlying connection for CQE dispatch chaining.
  IoUringConnection *connection() const { return conn_.get(); }

private:
  void send_register();
  void on_frame(uint8_t type, const uint8_t *data, size_t len);

  // Frame helpers
  static std::vector<uint8_t> make_frame(RouterMsg msg_type,
                                         const std::vector<uint8_t> &payload);
  static std::vector<uint8_t>
  make_publish_frame(const std::string &topic,
                     const std::vector<uint8_t> &payload);
  static std::vector<uint8_t>
  make_register_frame(const std::string &service_name,
                      const std::vector<std::string> &topics);
  static std::vector<uint8_t> make_subscribe_frame(const std::string &topic);

  std::unique_ptr<IoUringConnection> conn_;
  std::string host_;
  uint16_t port_ = 0;
  std::string service_name_;
  std::vector<std::string> pending_topics_;
  bool connected_ = false;
  bool registered_ = false;
  int connect_attempts_ = 0;
};

} // namespace gtnh::net
