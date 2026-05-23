#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "ITopicHandler.h"
#include "clients/IoUringRouterClient.h"

namespace simcore {

class TopicDispatcher {
public:
    void on(std::string topic, std::unique_ptr<ITopicHandler> handler);
    // Returns true if a handler was found and dispatched
    bool dispatch(const std::string& topic, const std::vector<uint8_t>& data);
    void subscribeAll(std::shared_ptr<IoUringRouterClient> client);

private:
    std::unordered_map<std::string, std::unique_ptr<ITopicHandler>> handlers_;
};

} // namespace simcore
