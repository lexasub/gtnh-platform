#include "ServiceHealthStore.h"

#include "service_health_generated.h"
#include <flatbuffers/verifier.h>

void ServiceHealthStore::Apply(std::shared_ptr<std::vector<uint8_t>> data) {
  if (!data) {
    return;
  }
  flatbuffers::Verifier verifier(data->data(), data->size());
  if (!verifier.VerifyBuffer<Protocol::ServiceHealthResp>(nullptr)) {
    return;
  }
  const auto *response = flatbuffers::GetRoot<Protocol::ServiceHealthResp>(data->data());
  if (!response) {
    return;
  }

  std::vector<ServiceHealthRow> next;
  const auto *services = response->services();
  if (services) {
    next.reserve(services->size());
    for (const auto *service : *services) {
      if (!service || !service->name()) {
        continue;
      }
      next.push_back(ServiceHealthRow{
          service->name()->str(), service->transport_alive(),
          static_cast<uint8_t>(service->probe_state()),
          service->last_response_age_ms()});
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  rows_ = std::move(next);
}

std::vector<ServiceHealthRow> ServiceHealthStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rows_;
}
