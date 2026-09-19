#pragma once
#include <cstdint>
#include <vector>

namespace simcore {

struct NetworkConnectionComponent {
  std::vector<uint32_t> network_ids;

  NetworkConnectionComponent() = default;
  explicit NetworkConnectionComponent(std::vector<uint32_t> ids)
      : network_ids(std::move(ids)) {}
};

} // namespace simcore
