#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Parsed service-health rows are owned by this render-thread store.
struct ServiceHealthRow {
  std::string name;
  bool transportAlive = false;
  uint8_t probeState = 0;
  uint64_t lastResponseAgeMs = 0;
};

class ServiceHealthStore {
public:
  void Apply(std::shared_ptr<std::vector<uint8_t>> data);
  std::vector<ServiceHealthRow> Snapshot() const;

private:
  mutable std::mutex mutex_;
  std::vector<ServiceHealthRow> rows_;
};
