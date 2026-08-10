#include "Network/ChestCloseHandler.h"
#include "Storage/ChestStateManager.h"
#include "Storage/ContainerSession.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

ChestCloseHandler::ChestCloseHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                                     std::shared_ptr<ChestStateManager> stateMgr)
    : sessions_(std::move(sessions))
    , stateMgr_(std::move(stateMgr)) {}

void ChestCloseHandler::handle(const std::vector<uint8_t>& data) {
  flatbuffers::Verifier v(data.data(), data.size());
  if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) return;
  auto* req = flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data.data());
  if (!req || !req->pos()) return;
  uint64_t pid = req->player_id();
  auto* p = req->pos();
  if (pid == 0) return;

  ContainerSession sess;
  if (!sessions_->get(pid, sess)) {
    spdlog::debug("[ChestClose] player={} close with no open session", pid);
    return;
  }

  spdlog::info("[ChestClose] player={} close chest at ({},{},{}) — persist {} slots",
               pid, p->x(), p->y(), p->z(), sess.slots.size());
  stateMgr_->saveSlots(p->x(), p->y(), p->z(), sess.slots);
  sessions_->close(pid);
}

} // namespace simcore
