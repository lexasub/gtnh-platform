#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtnh/net/router_client.h>

namespace simcore {

class IoUringRouterClient {
public:
  using MessageCallback = std::function<void(const std::string &topic,
                                             const std::vector<uint8_t> &data)>;

  IoUringRouterClient() = default;
  ~IoUringRouterClient() = default;

  IoUringRouterClient(const IoUringRouterClient &) = delete;
  IoUringRouterClient &operator=(const IoUringRouterClient &) = delete;
  IoUringRouterClient(IoUringRouterClient &&) = delete;
  IoUringRouterClient &operator=(IoUringRouterClient &&) = delete;

  void SetServiceName(const std::string &name);
  bool Connect(const std::string &host, uint16_t port);
  void Subscribe(const std::string &topic);
  void Publish(const std::string &topic, const std::vector<uint8_t> &payload);
  void PublishRaw(const std::string &topic, const uint8_t *data, size_t len);
  void OnMessage(MessageCallback callback);
  void Stop();
  bool IsConnected() const;
  void SendHeartbeat();

private:
  gtnh::net::RouterClient router_client_;
  std::string service_name_;
  MessageCallback on_message_;
};

} // namespace simcore
