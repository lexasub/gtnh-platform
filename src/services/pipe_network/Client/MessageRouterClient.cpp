#include "Client/MessageRouterClient.h"
#include <algorithm>
#include <cstring>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <arpa/inet.h>

namespace gtnh {
namespace pipe_network {

MessageRouterClient::MessageRouterClient(asio::io_context& io)
    : io_(io), socket_(io), heartbeat_timer_(io), write_strand_(io_.get_executor()), reading_(false)
{}

MessageRouterClient::~MessageRouterClient() { Stop(); }

void MessageRouterClient::SetServiceName(const std::string& name) { service_name_ = name; }

void MessageRouterClient::Connect(const std::string& host, uint16_t port) {
    host_ = host; port_ = port;
    if (!connected_) doConnect(host, port);
}

void MessageRouterClient::doConnect(const std::string& host, uint16_t port) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host), port);
    asio::error_code ignore;
    socket_.close(ignore);
    socket_.async_connect(endpoint, [this, host, port](asio::error_code ec) {
        if (ec) { spdlog::error("Failed to connect to router at {}:{}", host, port);
                  retryConnect(host, port); return; }
        connected_ = true;
        onConnect(ec);
    });
}

void MessageRouterClient::retryConnect(const std::string& host, uint16_t port) {
    if (!connected_) {
        spdlog::info("Retrying connection to router at {}:{}", host, port);
        asio::post(io_, [this, host, port]() { doConnect(host, port); });
    }
}

void MessageRouterClient::onConnect(const asio::error_code& ec) {
    if (ec) { spdlog::error("Connection failed: {}", ec.message()); return; }
    spdlog::info("Connected to message router");
    if (!stopped_) {
        registerClient();
        scheduleHeartbeat();
    }
    readFrame();
}

void MessageRouterClient::onDisconnect(const asio::error_code& ec) {
    if (!stopped_) {
        spdlog::warn("Disconnected from message router: {}", ec.message());
        connected_ = false;
        retryConnect(host_, port_);
    }
}

bool MessageRouterClient::onReadFrame(asio::error_code ec2, uint8_t msg_type, uint32_t data_len, std::shared_ptr<std::vector<uint8_t>> payload) {
    if (ec2) {
        reading_ = false;
        if (ec2 != asio::error::eof)
            spdlog::error("Router payload read error: {}", ec2.message());
        onDisconnect(ec2);
        return true;
    }

    if (msg_type != static_cast<uint8_t>(RouterMsg::kPublish)) {
        if (msg_type != static_cast<uint8_t>(RouterMsg::kHeartbeat)) {
            spdlog::trace("Router: unhandled msg type 0x{:02x} ({} bytes)",
                          msg_type, data_len);
        }

        reading_ = false;
        if (!stopped_) readFrame();
        return true;
    }
    // Publish payload: [2B topic_len BE] [topic] [data...]
    if (data_len < 2) {
        spdlog::warn("Router: publish frame too small ({} bytes)", data_len);

        reading_ = false;
        if (!stopped_) readFrame();
        return true;
    }
    uint16_t topic_len =
            (static_cast<uint16_t>((*payload)[0]) << 8) |
            (static_cast<uint16_t>((*payload)[1]));
    if (2u + topic_len > data_len) {
        spdlog::warn("Router: publish topic truncated "
                     "(len={}, data={})", topic_len, data_len);
        reading_ = false;
        if (!stopped_) readFrame();
        return true;
    }
    std::string topic(
        reinterpret_cast<const char *>(payload->data() + 2),
        topic_len);
    std::vector<uint8_t> msg_data(
        payload->begin() + 2 + topic_len,
        payload->end());
    handleMessage(topic, msg_data);

    reading_ = false;
    if (!stopped_) readFrame();
    return false;
}

void MessageRouterClient::readFrame() {
    if (reading_) return;
    reading_ = true;

    auto header = std::make_shared<std::array<uint8_t, FRAME_HEADER_SIZE>>();
    asio::async_read(socket_, asio::buffer(*header),
        [this, header](asio::error_code ec, size_t) {
            if (ec) {
                reading_ = false;
                if (ec != asio::error::eof)
                    spdlog::error("Read error: {}", ec.message());
                onDisconnect(ec);
                return;
            }

            // Parse header: [4B payload_len BE] [1B msg_type]
            uint32_t payload_len =
                (static_cast<uint32_t>((*header)[0]) << 24) |
                (static_cast<uint32_t>((*header)[1]) << 16) |
                (static_cast<uint32_t>((*header)[2]) << 8)  |
                (static_cast<uint32_t>((*header)[3]));
            uint8_t msg_type = (*header)[4];

            if (payload_len < 1) {
                spdlog::error("Router: invalid payload length {}", payload_len);
                reading_ = false;
                if (!stopped_) readFrame();
                return;
            }

            uint32_t data_len = payload_len - 1; // exclude the msg_type byte

            // Zero-length payload (heartbeats, empty acks, etc.)
            if (data_len == 0) {
                if (msg_type == static_cast<uint8_t>(RouterMsg::kPublish)) {
                    spdlog::warn("Router: empty publish frame");
                } else if (msg_type != static_cast<uint8_t>(RouterMsg::kHeartbeat)) {
                    spdlog::trace("Router: unhandled msg type 0x{:02x} (empty)", msg_type);
                }
                reading_ = false;
                if (!stopped_) readFrame();
                return;
            }

            auto payload = std::make_shared<std::vector<uint8_t>>(data_len);
            asio::async_read(socket_, asio::buffer(*payload),
                [this, payload, msg_type, data_len](asio::error_code ec2, size_t) {
                    onReadFrame(ec2, msg_type, data_len, payload);
                });
        });
}


void MessageRouterClient::writeFrame(const std::vector<uint8_t> &frame) {
    if (frame.empty() || !socket_.is_open()) return;
    EnqueueWrite(std::make_shared<std::vector<uint8_t> >(frame));
}

void MessageRouterClient::EnqueueWrite(std::shared_ptr<std::vector<uint8_t>> frame) {
    asio::post(write_strand_, [this, frame = std::move(frame)]() mutable {
        bool was_empty = write_queue_.empty();
        write_queue_.push_back({std::move(frame)});
        if (was_empty) {
            DoWrite();
        }
    });
}

void MessageRouterClient::DoWrite() {
    if (write_queue_.empty() || write_in_progress_) return;
    if (!socket_.is_open()) {
        write_queue_.clear();
        return;
    }

    write_in_progress_ = true;
    auto op = std::move(write_queue_.front());
    write_queue_.pop_front();
    auto buf = asio::buffer(*op);
    asio::async_write(socket_, buf,
        asio::bind_executor(write_strand_, [this, op = std::move(op)](const asio::error_code& ec, std::size_t bytes) {
            write_in_progress_ = false;
            if (ec && ec != asio::error::operation_aborted) {
                spdlog::error("Write error: {} ({} bytes)", ec.message(), bytes);
            }
            DoWrite();
        }));
}

void MessageRouterClient::Disconnect() {
    if (connected_) {
        asio::error_code ec;
        socket_.close(ec);
        connected_ = false;
    }
}

void MessageRouterClient::Register() {
    registerClient();
}

void MessageRouterClient::Subscribe(const std::string& topic) {
    subscribed_topics_.push_back(topic);
    if (connected_) {
        subscribeToTopic(topic);
    }
}

void MessageRouterClient::Unsubscribe(const std::string& topic) {
    auto it = std::find(subscribed_topics_.begin(), subscribed_topics_.end(), topic);
    if (it != subscribed_topics_.end()) {
        subscribed_topics_.erase(it);
    }
    if (connected_) {
        unsubscribeFromTopic(topic);
    }
}

void MessageRouterClient::Publish(const std::string& topic, const std::vector<uint8_t>& payload) {
    if (!connected_) return;
    publishToTopic(topic, payload);
}

void MessageRouterClient::PublishRaw(const std::string& topic, const uint8_t* data, size_t len) {
    if (!connected_) return;
    std::vector<uint8_t> payload(data, data + len);
    publishToTopic(topic, payload);
}

void MessageRouterClient::OnMessage(MessageCallback callback) {
    on_message_ = std::move(callback);
}

std::vector<uint8_t> MessageRouterClient::decodePublishFrame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) return {};
    uint16_t topicLen = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    if (topicLen + 2u > frame.size()) return {};
    return std::vector<uint8_t>(frame.begin() + 2 + topicLen, frame.end());
}

void MessageRouterClient::registerClient() {
    if (service_name_.empty()) {
        spdlog::error("Cannot register: service name not set. Call SetServiceName() before Connect().");
        return;
    }
    writeFrame(makeRegisterFrame(service_name_, subscribed_topics_));
}

void MessageRouterClient::subscribeToTopic(const std::string& topic) {
    writeFrame(makeSubscribeFrame(topic));
}

void MessageRouterClient::unsubscribeFromTopic(const std::string& topic) {
    writeFrame(makeUnsubscribeFrame(topic));
}

void MessageRouterClient::publishToTopic(const std::string& topic, const std::vector<uint8_t>& payload) {
    writeFrame(makePublishFrame(topic, payload));
}

void MessageRouterClient::handleMessage(const std::string& topic, const std::vector<uint8_t>& data) {
    spdlog::trace("Router: message on '{}' ({} bytes)", topic, data.size());
    if (on_message_) {
        on_message_(topic, data);
    }
}

void MessageRouterClient::scheduleHeartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(HEARTBEAT_INTERVAL_SEC));
    heartbeat_timer_.async_wait([this](std::error_code ec) { doHeartbeat(ec); });
}

void MessageRouterClient::doHeartbeat(std::error_code ec) {
    if (ec) return;
    if (stopped_) return;
    uint8_t frame[5];
    frame[0] = 0; frame[1] = 0; frame[2] = 0; frame[3] = 1;
    frame[4] = static_cast<uint8_t>(RouterMsg::kHeartbeat);
    writeFrame(std::vector<uint8_t>(frame, frame + 5));
    scheduleHeartbeat();
}

std::vector<uint8_t> MessageRouterClient::makeFrame(RouterMsg msgType, const std::vector<uint8_t>& payload) {
    // Wire format: [4B: payload_len (incl. msgType)][1B: msgType][payload]
    // payload_len = 1 (msgType) + payload.size()
    // frame size = 4 (len) + 1 (type) + payload.size() = 5 + payload.size()
    size_t payloadLen = 1 + payload.size();
    std::vector<uint8_t> frame(FRAME_HEADER_SIZE + payload.size());
    frame[0] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
    frame[1] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    frame[2] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    frame[3] = static_cast<uint8_t>(payloadLen & 0xFF);
    frame[4] = static_cast<uint8_t>(msgType);
    std::copy(payload.begin(), payload.end(), frame.begin() + 5);
    return frame;
}

std::vector<uint8_t> MessageRouterClient::makePublishFrame(const std::string& topic, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> topic_prefixed;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    topic_prefixed.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));  // big-endian high byte
    topic_prefixed.push_back(static_cast<uint8_t>(topic_len & 0xFF));         // big-endian low byte
    topic_prefixed.insert(topic_prefixed.end(), topic.begin(), topic.end());
    topic_prefixed.insert(topic_prefixed.end(), payload.begin(), payload.end());
    return makeFrame(RouterMsg::kPublish, topic_prefixed);
}

std::vector<uint8_t> MessageRouterClient::makeRegisterFrame(const std::string& serviceName, const std::vector<std::string>& topics) {
    std::vector<uint8_t> payload;
    uint16_t name_len = static_cast<uint16_t>(serviceName.size());
    payload.push_back(static_cast<uint8_t>((name_len >> 8) & 0xFF));  // big-endian high byte
    payload.push_back(static_cast<uint8_t>(name_len & 0xFF));         // big-endian low byte
    payload.insert(payload.end(), serviceName.begin(), serviceName.end());
    uint16_t ntopics = static_cast<uint16_t>(topics.size());
    payload.push_back(static_cast<uint8_t>((ntopics >> 8) & 0xFF));   // big-endian high byte
    payload.push_back(static_cast<uint8_t>(ntopics & 0xFF));          // big-endian low byte
    for (const auto& t : topics) {
        uint16_t topic_len = static_cast<uint16_t>(t.size());
        payload.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));  // big-endian high byte
        payload.push_back(static_cast<uint8_t>(topic_len & 0xFF));         // big-endian low byte
        payload.insert(payload.end(), t.begin(), t.end());
    }
    return makeFrame(RouterMsg::kRegister, payload);
}

std::vector<uint8_t> MessageRouterClient::makeSubscribeFrame(const std::string& topic) {
    std::vector<uint8_t> payload;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    payload.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));  // big-endian high byte
    payload.push_back(static_cast<uint8_t>(topic_len & 0xFF));         // big-endian low byte
    payload.insert(payload.end(), topic.begin(), topic.end());
    return makeFrame(RouterMsg::kSubscribe, payload);
}

std::vector<uint8_t> MessageRouterClient::makeUnsubscribeFrame(const std::string& topic) {
    std::vector<uint8_t> payload;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    payload.push_back(static_cast<uint8_t>((topic_len >> 8) & 0xFF));  // big-endian high byte
    payload.push_back(static_cast<uint8_t>(topic_len & 0xFF));         // big-endian low byte
    payload.insert(payload.end(), topic.begin(), topic.end());
    return makeFrame(RouterMsg::kUnsubscribe, payload);
}


void MessageRouterClient::Stop() {
    if (!stopped_) {
        stopped_ = true;
        asio::error_code ec;
        heartbeat_timer_.cancel(ec);
        if (connected_) {
            socket_.close(ec);
        }
    }
}

bool MessageRouterClient::IsConnected() const { return connected_; }

} // namespace pipe_network
} // namespace gtnh
