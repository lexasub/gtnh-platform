#include "IoUringRouterClient.h"
#include <spdlog/spdlog.h>

namespace simcore {

void IoUringRouterClient::SetServiceName(const std::string& name) {
    service_name_ = name;
}

bool IoUringRouterClient::Connect(const std::string& host, uint16_t port) {
    if (!router_client_.connect(host.c_str(), port, service_name_.c_str())) {
        spdlog::error("IoUringRouterClient: connect to {}:{} failed", host, port);
        return false;
    }
    spdlog::info("IoUringRouterClient: connected to {}:{} as '{}'", host, port, service_name_);
    return true;
}

void IoUringRouterClient::Subscribe(const std::string& topic) {
    router_client_.subscribe(topic);
}

void IoUringRouterClient::Publish(const std::string& topic, const std::vector<uint8_t>& payload) {
    router_client_.publish(topic, payload);
}

void IoUringRouterClient::PublishRaw(const std::string& topic, const uint8_t* data, size_t len) {
    router_client_.publish(topic, data, len);
}

void IoUringRouterClient::OnMessage(MessageCallback callback) {
    on_message_ = std::move(callback);
    router_client_.on_publish = [this](const std::string& topic,
                                        std::shared_ptr<std::vector<uint8_t>> data) {
        if (on_message_) {
            on_message_(topic, *data);
        }
    };
}

void IoUringRouterClient::Stop() {
    router_client_.disconnect();
}

bool IoUringRouterClient::IsConnected() const {
    return router_client_.is_connected();
}

void IoUringRouterClient::SendHeartbeat() {
    router_client_.heartbeat();
}

} // namespace simcore
