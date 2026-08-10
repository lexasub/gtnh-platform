#include "Network/MachineCloseHandler.h"
#include "Storage/ChestStateManager.h"
#include "Storage/ContainerSession.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

MachineCloseHandler::MachineCloseHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                                         std::shared_ptr<ChestStateManager> stateMgr)
    : sessions_(std::move(sessions))
    , stateMgr_(std::move(stateMgr)) {}

void MachineCloseHandler::handle(const std::vector<uint8_t>& data) {
  flatbuffers::Verifier v(data.data(), data.size());
  if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) return;
  auto* req = flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data.data());
  if (!req || !req->pos()) return;
  uint64_t pid = req->player_id();
  auto* p = req->pos();
  if (pid == 0) return;

  ContainerSession* sess = sessions_->find(pid);
  if (!sess || !sess->isMachine()) {
    spdlog::debug("[MachineClose] player={} close with no open machine session", pid);
    return;
  }

  auto* slots = sess->slotsRef();
  spdlog::info("[MachineClose] player={} close machine at ({},{},{}) — persist {} slots",
               pid, p->x(), p->y(), p->z(), slots ? slots->size() : 0);
  if (slots) {
    stateMgr_->saveSlots(sess->x, sess->y, sess->z, *slots, sess->entity_type);
  }
  sessions_->close(pid);
}

} // namespace simcore
