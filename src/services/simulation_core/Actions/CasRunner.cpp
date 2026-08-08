#include "Actions/CasRunner.h"
#include "Network/IEventPublisher.h"
#include "Storage/IBlockRepository.h"
#include <chrono>
#include <spdlog/spdlog.h>

namespace simcore {

#define TRACE_LOG(tid, svc, op, dur_us) \
    spdlog::info("[TRACE tid={}] {} {} {}us", (tid), (svc), (op), (dur_us))

void runBlockCas(const ActionContext& ctx, int32_t eff_x, int32_t eff_y,
                 int32_t eff_z, uint16_t eff_expected, uint16_t final_block_id,
                 uint8_t final_meta, std::function<void()> onCommitted) {
  ctx.publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
      eff_x, eff_y, eff_z, final_block_id, final_meta, nullptr, ctx.request_id,
      static_cast<uint8_t>(ctx.action_type));

  // Everything the async callback needs — copied, no references.
  auto repo = ctx.repo_;
  auto publisher = ctx.publisher_;
  auto postToMain = ctx.postToMain_;
  uint32_t request_id = ctx.request_id;
  uint8_t action_type = ctx.action_type;

  auto cas_t0 = std::chrono::steady_clock::now();
  repo->setBlockCAS(eff_x, eff_y, eff_z, eff_expected, final_block_id, final_meta,
      [repo, publisher, postToMain, request_id, action_type, eff_x, eff_y, eff_z,
       eff_expected, final_block_id, final_meta, onCommitted = std::move(onCommitted),
       cas_t0](const CASResult& result) {
        auto cas_dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - cas_t0).count();
        TRACE_LOG(request_id, "cas_cb", "complete", cas_dur);
        auto processResult = [publisher, request_id, action_type, onCommitted,
                              eff_x, eff_y, eff_z, eff_expected, final_block_id,
                              final_meta](const CASResult& result) {
          if (result.status == 0) {
            spdlog::info("Block CAS OK at ({},{},{}) final_id={}",
                         eff_x, eff_y, eff_z, final_block_id);
            if (onCommitted) onCommitted();
          } else {
            spdlog::warn("Block CAS CONFLICT at ({},{},{}) actual_id={}, from_id={}, to_id={}",
                         eff_x, eff_y, eff_z, result.block_id, eff_expected, final_block_id);
            publisher->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_CONFLICT),
                                       eff_x, eff_y, eff_z, result.block_id, result.meta,
                                       nullptr, request_id, action_type);
          }
        };
        if (postToMain) {
          postToMain([processResult, result]() { processResult(result); });
        } else {
          processResult(result);
        }
      });
}

#undef TRACE_LOG

} // namespace simcore
