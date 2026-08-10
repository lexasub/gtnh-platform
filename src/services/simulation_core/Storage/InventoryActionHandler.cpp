#include "InventoryActionHandler.h"
#include "InventoryClick.h"
#include "PlayerInventoryStore.h"
#include "ContainerSession.h"
#include "ChestStateManager.h"
#include "Crafting/WorkbenchStateManager.h"
#include "Quest/QuestManager.h"
#include "MachineRegistry.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
InventoryActionHandler::InventoryActionHandler(std::shared_ptr<PlayerInventoryStore> inv,
                                               std::shared_ptr<IoUringRouterClient> r,
                                               std::shared_ptr<ContainerSessionRegistry> chestSessions,
                                               std::shared_ptr<ChestStateManager> chestStateManager,
                                               std::shared_ptr<QuestManager> questManager,
                                               std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager)
    : inventoryStore_(std::move(inv)), router_(std::move(r)),
      chestSessions_(std::move(chestSessions)),
      sessionsStateMgr_(std::move(chestStateManager)),
      questManager_(std::move(questManager)),
      wbStateManager_(std::move(wbStateManager)) {}

// PlayerInventoryStore::setSlotsAndCursor fires postMutation_, which main.cpp
// wires to publish the container_id=0 player-only snapshot — so a player-only
// click needs no explicit publish here. A container (chest) click additionally
// publishes the full container_id=1 snapshot (cursor + player + chest slots)
// and persists the chest to EntityStateStore (live per-action).
void InventoryActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::InventoryAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::InventoryAction>(data.data());
    if (!action) return;

    uint64_t pid = action->player_id();
    if (pid == 0) return;

    ContainerClick click{};
    click.action_type = action->action_type();
    click.button = action->button();
    click.mods = action->mods();
    click.container_id = action->container_id();
    click.slot = action->slot();
    click.count = action->count();

    // OH3 gate: a container click before the open-session load registers is
    // dropped (the client cannot normally click before the open snapshot).
    ContainerSession* sess = nullptr;
    if (click.container_id != 0) {
        if (!chestSessions_ || !(sess = chestSessions_->find(pid))) {
            spdlog::warn("[SimCore] InventoryAction container_id={} before session load (pid={}) — dropped",
                         click.container_id, pid);
            return;
        }
    }

    auto slots = inventoryStore_->getSlots(pid);
    auto cursor = inventoryStore_->getCursor(pid);

    // Phase C: a machine session mutates the LIVE ECS InventoryContainer via
    // slotsRef() (re-resolves try_get on every access); chest sessions mutate
    // the owned copy. Layout compatibility is enforced in ContainerSession.h.
    InventoryRef inv{&slots, (click.container_id == 1) ? sess->slotsRef() : nullptr};

    // Machine removed/unloaded while the window was open: slotsRef() is null.
    // Drop the click without touching player state (authoritative no-op).
    if (click.container_id == 1 && !inv.container) {
        spdlog::warn("[SimCore] InventoryAction container_id=1 dropped: container gone "
                     "(machine removed/unloaded while window open, pid={})", pid);
        return;
    }

    // S5b: quest detection. Snapshot the clicked machine OUTPUT slot before
    // the click — when the player takes a produced item we report it to
    // QuestManager (mirrors MachineSlotHandler: item_id + taken count).
    bool questCandidate = false;
    uint16_t questItem = 0;
    uint8_t questCount = 0;
    if (questManager_ && click.container_id == 1 && sess->isMachine() &&
        click.action_type != kActionDrop) {
        // Drop destroys the item (does not reach the player) — no quest credit.
        const MachineInfo* minfo = MachineRegistry::instance()
                                       ? MachineRegistry::instance()->Get(sess->entity_type)
                                       : nullptr;
        if (minfo && click.slot >= static_cast<uint16_t>(minfo->slots_in) &&
            click.slot < static_cast<uint16_t>(kInventorySlots)) {
            if (auto* src = SlotAt(inv, 1, click.slot)) {
                questItem = src->item_id;
                questCount = src->count;
                questCandidate = questItem != 0;
            }
        }
    }

    bool changed = ApplyContainerClick(inv, cursor, click);
    if (!changed) return;

    inventoryStore_->setSlotsAndCursor(pid, slots, cursor); // player-only publish (container_id=0)

    // S5b: the item left the machine output slot → it went to the player
    // (pick-up to cursor, swap, or quick-move to inventory). Report to quests.
    if (questCandidate) {
        if (auto* src = SlotAt(inv, 1, click.slot)) {
            if (src->item_id != questItem || src->count < questCount) {
                questManager_->checkMachineOutput(pid, sess->entity_type, questItem,
                                                  questCount);
            }
        }
    }

    if (click.container_id == 1 && sess) {
        // Authoritative full snapshot (container_id=1) + live per-action persist.
        PublishFullInventory(router_, *inventoryStore_, *chestSessions_, pid,
                             sess->x, sess->y, sess->z);
        if (sess->kind == ContainerSession::Kind::Workbench) {
            // Workbench grid is not an EntityStateStore container — it lives in
            // WorkbenchStateManager (cache + ESS), keyed by block position.
            if (wbStateManager_) {
                if (auto* persistRef = sess->slotsRef()) {
                    std::vector<RecipeManager::ItemStack> grid;
                    grid.reserve(persistRef->size());
                    for (const auto& ps : *persistRef) {
                        grid.emplace_back(
                            RecipeManager::ItemStack{ps.item_id, ps.count, ps.meta});
                    }
                    wbStateManager_->setGridState(sess->x, sess->y, sess->z, grid);
                }
            }
        } else if (sessionsStateMgr_) {
            // Machine: entity_type = machine_id (ESS key); chest: kChestEntityType.
            // Skip the save if the machine was removed mid-click (slotsRef() null).
            if (auto* persistRef = sess->slotsRef()) {
                sessionsStateMgr_->saveSlots(sess->x, sess->y, sess->z, *persistRef,
                                             sess->isMachine() ? sess->entity_type
                                                               : kChestEntityType);
            }
        }
    }
    spdlog::debug("[SimCore] InventoryAction applied: pid={} act={} btn={} cid={} slot={}",
                  pid, click.action_type, click.button, click.container_id, click.slot);
}
} // namespace simcore
