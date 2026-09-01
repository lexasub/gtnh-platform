#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace simcore {

struct ITopicHandler {
  virtual ~ITopicHandler() = default;
  virtual void handle(const std::vector<uint8_t> &data) = 0;
};

// Adapter for exposing several methods of one shared handler object as
// distinct TopicDispatcher entries (TopicDispatcher owns one unique_ptr per
// topic, so a single object cannot be registered directly more than once).
class FnTopicHandler final : public ITopicHandler {
public:
  using Fn = std::function<void(const std::vector<uint8_t> &)>;
  explicit FnTopicHandler(Fn fn) : fn_(std::move(fn)) {}
  void handle(const std::vector<uint8_t> &data) override { fn_(data); }

private:
  Fn fn_;
};

} // namespace simcore
