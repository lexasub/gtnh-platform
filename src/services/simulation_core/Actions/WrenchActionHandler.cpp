#include "WrenchActionHandler.h"
#include "WrenchHandler.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
WrenchActionHandler::WrenchActionHandler(std::shared_ptr<WrenchHandler> wrenchHandler)
    : wrenchHandler_(std::move(wrenchHandler)) {}

void WrenchActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::ToolAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::ToolAction>(data.data());
    if (!action || !action->pos()) return;
    auto* p = action->pos();
    
    if (action->action() != Protocol::ToolActionType_WRENCH_CYCLE) return;
    
    auto r = wrenchHandler_->cycleFace(action->player_id(), p->x(), p->y(), p->z(), action->face());
    
    flatbuffers::FlatBufferBuilder fbb(128);
    auto err = r.error.empty() ? 0 : fbb.CreateString(r.error);
    auto roles = fbb.CreateVector(r.allRoles, 6);
    auto resp = Protocol::CreateToolActionResp(fbb, r.success, err, 0, 0, r.newRole, roles);
    fbb.Finish(resp);
    std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    
    router_->Publish("player.tool.action.response", std::move(respData));
}
} // namespace simcore