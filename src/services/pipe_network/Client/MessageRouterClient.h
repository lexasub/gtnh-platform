#pragma once
#include <asio.hpp>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gtnh {
namespace pipe_network {

// Router message types — must match Go message_router/router.go MsgType values
enum class RouterMsg : uint8_t {
  kSubscribe = 0x01,
  kUnsubscribe = 0x02,
  kPublish = 0x03,
  kRegister = 0x04,
  kHeartbeat = 0x05,
};

// Frame header: [4 bytes: payload size] [1 byte: msg type]
constexpr size_t FRAME_HEADER_SIZE = 5;

class MessageRouterClient {
public:
  using MessageCallback = std::function<void(const std::string &topic,
                                             const std::vector<uint8_t> &data)>;

  explicit MessageRouterClient(asio::io_context &io);
  ~MessageRouterClient();

  void SetServiceName(const std::string &name);
  void Connect(const std::string &host, uint16_t port);
  void Disconnect();
  void Register();
  void Subscribe(const std::string &topic);
  void Unsubscribe(const std::string &topic);
  void Publish(const std::string &topic, const std::vector<uint8_t> &payload);
  void PublishRaw(const std::string &topic, const uint8_t *data, size_t len);
  void OnMessage(MessageCallback callback);
  void Stop();
  bool IsConnected() const;

private:
  void doConnect(const std::string &host, uint16_t port);
  void retryConnect(const std::string &host, uint16_t port);
  void onConnect(const asio::error_code &ec);
  void onDisconnect(const asio::error_code &ec);

  bool onReadFrame(asio::error_code ec2, uint8_t msg_type, uint32_t data_len,
                   std::shared_ptr<std::vector<uint8_t>> payload);

  void writeFrame(const std::vector<uint8_t> &frame);
  void readFrame();
  void scheduleHeartbeat();
  void doHeartbeat(std::error_code ec);

  void registerClient();
  void subscribeToTopic(const std::string &topic);
  void unsubscribeFromTopic(const std::string &topic);
  void publishToTopic(const std::string &topic,
                      const std::vector<uint8_t> &payload);
  void handleMessage(const std::string &topic,
                     const std::vector<uint8_t> &data);

  static std::vector<uint8_t> makeFrame(RouterMsg msgType,
                                        const std::vector<uint8_t> &payload);
  static std::vector<uint8_t>
  makePublishFrame(const std::string &topic,
                   const std::vector<uint8_t> &payload);
  static std::vector<uint8_t>
  makeRegisterFrame(const std::string &serviceName,
                    const std::vector<std::string> &topics);
  static std::vector<uint8_t> makeSubscribeFrame(const std::string &topic);
  static std::vector<uint8_t> makeUnsubscribeFrame(const std::string &topic);
  static std::vector<uint8_t>
  decodePublishFrame(const std::vector<uint8_t> &frame);

  // Write serialization: prevents concurrent async_write on the same socket
  void EnqueueWrite(std::shared_ptr<std::vector<uint8_t>> frame);
  void DoWrite();

  asio::io_context &io_;
  asio::ip::tcp::socket socket_;
  asio::steady_timer heartbeat_timer_;
  asio::strand<asio::io_context::executor_type> write_strand_;
  std::deque<std::shared_ptr<std::vector<uint8_t>>> write_queue_;
  bool write_in_progress_ = false;
  std::string host_, service_name_;
  uint16_t port_ = 0;
  bool connected_ = false, stopped_ = false;
  MessageCallback on_message_;
  std::vector<std::string> subscribed_topics_;
  std::atomic<bool> reading_;
  static constexpr int HEARTBEAT_INTERVAL_SEC = 20;
};

} // namespace pipe_network
} // namespace gtnh
