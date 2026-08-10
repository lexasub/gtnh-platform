#include "PipeWrenchResponseHandler.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

PipeWrenchResponseHandler::PipeWrenchResponseHandler(std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router)) {}

void PipeWrenchResponseHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::PipeWrenchResp>(nullptr)) return;
    const auto* resp = flatbuffers::GetRoot<Protocol::PipeWrenchResp>(data.data());
    if (!resp || !resp->pos()) return;

    std::string message;
    switch (resp->guidance()) {
        case Protocol::PipeWrenchGuidance_CONNECT_PIPES:
            message = "Pipe is not connected. Place adjacent pipes to build a network.";
            break;
        case Protocol::PipeWrenchGuidance_CONNECT_TO_MACHINE:
            message = "Pipe has no machine connection. Place the pipe next to a machine to attach it.";
            break;
        case Protocol::PipeWrenchGuidance_CONNECTED:
            message = "Pipe is connected to a network (" +
                      std::to_string(resp->component_size()) + " segments).";
            break;
        case Protocol::PipeWrenchGuidance_NOT_A_PIPE:
        default:
            message = "Not a pipe.";
            break;
    }

    flatbuffers::FlatBufferBuilder fbb(128);
    auto msg = fbb.CreateString(message);
    auto out = Protocol::CreateToolActionResp(fbb, true, 0, 0, 0, 0, 0, msg);
    fbb.Finish(out);
    std::vector<uint8_t> buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    router_->Publish("player.tool.action.response", std::move(buf));
}

} // namespace simcore
