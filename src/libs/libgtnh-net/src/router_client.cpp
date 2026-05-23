#include <gtnh/net/router_client.h>
#include <gtnh/net/frame.h>

#include <spdlog/spdlog.h>
#include <cstring>

namespace gtnh::net {

// =========================================================================
//  Lifecycle
// =========================================================================

bool RouterClient::connect(const char* host, uint16_t port,
                            const char* service_name) {
    if (connected_) return true;

    host_ = host;
    port_ = port;
    service_name_ = service_name;

    int fd = TcpConnector::connect(host, port);
    if (fd < 0) {
        spdlog::error("RouterClient: connect to {}:{} failed", host, port);
        return false;
    }

    static TagAllocator tag_alloc;
    ConnectionTags tags = tag_alloc.alloc();
    conn_ = std::make_unique<IoUringConnection>(fd, "router", tags);

    conn_->on_message = [this](uint8_t msg_type, const uint8_t* data, size_t len) {
        on_frame(msg_type, data, len);
    };
    conn_->on_closed = [this]() {
        connected_ = false;
        registered_ = false;
        spdlog::warn("RouterClient: connection closed");
    };

    if (!conn_->start_reading()) {
        spdlog::error("RouterClient: failed to start reading");
        conn_->close();
        conn_.reset();
        return false;
    }

    connected_ = true;
    connect_attempts_ = 0;
    spdlog::info("RouterClient: connected to {}:{} as '{}'", host, port, service_name_);

    // Send pending subscribes collected before connect completed
    send_register();

    return true;
}

void RouterClient::disconnect() {
    connected_ = false;
    registered_ = false;
    if (conn_) {
        conn_->close();
        conn_.reset();
    }
}

// =========================================================================
//  Router protocol
// =========================================================================

void RouterClient::send_register() {
    if (service_name_.empty()) {
        spdlog::error("RouterClient: cannot register - service name not set");
        return;
    }

    spdlog::info("RouterClient::sendRegister: name='{}' topics={}",
                 service_name_, pending_topics_.size());

    std::vector<uint8_t> frame = make_register_frame(service_name_, pending_topics_);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
    registered_ = true;
    spdlog::info("RouterClient: registered as '{}'", service_name_);
}

void RouterClient::subscribe(const std::string& topic) {
    spdlog::info("RouterClient::subscribe('{}') connected={}", topic, connected_);

    if (!connected_) {
        pending_topics_.push_back(topic);
        spdlog::info("RouterClient: deferred subscribe to '{}'", topic);
        return;
    }

    std::vector<uint8_t> frame = make_subscribe_frame(topic);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
    spdlog::info("RouterClient: subscribed to '{}'", topic);
}

void RouterClient::publish(const std::string& topic, const uint8_t* data, size_t len) {
    if (!connected_) return;
    std::vector<uint8_t> payload(data, data + len);
    publish(topic, payload);
}

void RouterClient::publish(const std::string& topic, const std::vector<uint8_t>& data) {
    if (!connected_) return;
    std::vector<uint8_t> frame = make_publish_frame(topic, data);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
}

void RouterClient::heartbeat() {
    if (!connected_) return;
    auto frame = frame::pack_router(static_cast<uint8_t>(RouterMsg::kHeartbeat),
                                     nullptr, 0);
    conn_->send_raw(std::move(frame));
}

// =========================================================================
//  Read callback — called from IoUringContext poll thread
// =========================================================================

void RouterClient::on_frame(uint8_t msg_type, const uint8_t* data, size_t len) {
    switch (static_cast<RouterMsg>(msg_type)) {
    case RouterMsg::kPublish: {
        if (len < 2) {
            spdlog::warn("Router: publish frame too short ({} bytes)", len);
            return;
        }
        uint16_t topic_len = (static_cast<uint16_t>(data[0]) << 8) |
                             (static_cast<uint16_t>(data[1]));
        if (static_cast<size_t>(2 + topic_len) > len) {
            spdlog::warn("Router: publish topic truncated (topic_len={}, frame={})",
                         topic_len, len);
            return;
        }

        std::string topic(reinterpret_cast<const char*>(data + 2), topic_len);
        auto msg_data = std::make_shared<std::vector<uint8_t>>(
            data + 2 + topic_len, data + len);

        if (on_publish) {
            on_publish(topic, std::move(msg_data));
        }
        break;
    }
    case RouterMsg::kHeartbeat:
        break;
    default:
        spdlog::trace("Router: unhandled msg type 0x{:02x} ({} bytes)",
                      msg_type, len);
        break;
    }
}

// =========================================================================
//  Frame builders
// =========================================================================

std::vector<uint8_t> RouterClient::make_frame(RouterMsg msg_type,
                                               const std::vector<uint8_t>& payload) {
    uint32_t payload_len = 1 + static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> frame(4 + payload_len);
    frame::write_be32(frame.data(), payload_len);
    frame[4] = static_cast<uint8_t>(msg_type);
    if (!payload.empty()) {
        std::memcpy(frame.data() + 5, payload.data(), payload.size());
    }
    return frame;
}

std::vector<uint8_t> RouterClient::make_publish_frame(
    const std::string& topic, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> topic_prefixed;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    topic_prefixed.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    topic_prefixed.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    topic_prefixed.insert(topic_prefixed.end(), topic.begin(), topic.end());
    topic_prefixed.insert(topic_prefixed.end(), payload.begin(), payload.end());
    return make_frame(RouterMsg::kPublish, topic_prefixed);
}

std::vector<uint8_t> RouterClient::make_register_frame(
    const std::string& service_name, const std::vector<std::string>& topics) {
    std::vector<uint8_t> payload;
    uint16_t name_len = static_cast<uint16_t>(service_name.size());
    payload.push_back(static_cast<uint8_t>((name_len >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(name_len & 0xFF));
    payload.insert(payload.end(), service_name.begin(), service_name.end());

    uint16_t ntopics = static_cast<uint16_t>(topics.size());
    payload.push_back(static_cast<uint8_t>((ntopics >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(ntopics & 0xFF));
    for (const auto& t : topics) {
        uint16_t topic_len = static_cast<uint16_t>(t.size());
        payload.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(topic_len & 0xFF));
        payload.insert(payload.end(), t.begin(), t.end());
    }
    return make_frame(RouterMsg::kRegister, payload);
}

std::vector<uint8_t> RouterClient::make_subscribe_frame(const std::string& topic) {
    std::vector<uint8_t> payload;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    payload.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    payload.insert(payload.end(), topic.begin(), topic.end());
    return make_frame(RouterMsg::kSubscribe, payload);
}

} // namespace gtnh::net
