#include "TopicDispatcher.h"
#include <spdlog/spdlog.h>

namespace simcore {

void TopicDispatcher::on(std::string topic, std::unique_ptr<ITopicHandler> handler) {
    handlers_[std::move(topic)] = std::move(handler);
}

bool TopicDispatcher::dispatch(const std::string& topic, const std::vector<uint8_t>& data) {
    auto it = handlers_.find(topic);
    if (it != handlers_.end()) {
        it->second->handle(data);
        return true;
    }
    return false;
}

void TopicDispatcher::subscribeAll(std::shared_ptr<IoUringRouterClient> client) {
    for (const auto& [topic, handler] : handlers_) {
        client->Subscribe(topic);
    }
}

} // namespace simcore
