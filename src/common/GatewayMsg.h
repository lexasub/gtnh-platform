#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Gateway ↔ Client wire message types (frame: [4B size BE][1B type][FlatBuffer]).
//
// Single source of truth shared by the gateway (src/services/gateway/gateway.h)
// and the game client (src/services/game_client/Network/NetClient.h). The two
// services previously duplicated these constants and had already drifted
// (kEntitySnapshot vs kEntitySnap); a cross-service fixture test pins the
// client-facing types. Wire truth = these C++ constants; the FlatBuffers
// GatewayPayload union in gateway.fbs is stale and unused.
// ---------------------------------------------------------------------------
namespace GatewayMsg {
inline constexpr uint8_t kPlayerAction = 1;
inline constexpr uint8_t kChunkSnapshot = 2;
inline constexpr uint8_t kEntitySnapshot = 3;
inline constexpr uint8_t kBlockUpdate = 4;
inline constexpr uint8_t kBlockAck = 5;
inline constexpr uint8_t kInventoryUpdate = 6;
inline constexpr uint8_t kInventoryAction = 7;
inline constexpr uint8_t kBlockEntityUpdate = 8;
inline constexpr uint8_t kCraftRequest = 9;
inline constexpr uint8_t kCraftResponse = 10;
inline constexpr uint8_t kSetBlockAction = 11;
inline constexpr uint8_t kCompressedChunkData = 12;
inline constexpr uint8_t kToolAction = 13;
inline constexpr uint8_t kToolActionResp = 14;
inline constexpr uint8_t kSetMachineSlot = 15;
inline constexpr uint8_t kSetMachineSlotResp = 16;
inline constexpr uint8_t kRecipeCompleted = 17;
inline constexpr uint8_t kMachineOpenReq = 18; // was kChestSaveReq (dead, removed)
inline constexpr uint8_t kChestOpenReq = 19;
inline constexpr uint8_t kChestCloseReq = 45;
inline constexpr uint8_t kMachineCloseReq = 46;
inline constexpr uint8_t kQuestProgressUpdate = 20;
inline constexpr uint8_t kQuestUnlockNotification = 21;
inline constexpr uint8_t kQuestCompletedNotification = 22;
inline constexpr uint8_t kMultiblockEvent = 23;
inline constexpr uint8_t kQuestCompleteRequest = 24;
inline constexpr uint8_t kQuestEraTransition = 25;
inline constexpr uint8_t kQuestExchangeRequest = 26;
inline constexpr uint8_t kQuestExchangeResponse = 27;
inline constexpr uint8_t kQuestExchangeCooldownGet = 28;
inline constexpr uint8_t kQuestExchangeCooldown = 29;
inline constexpr uint8_t kGameModeChange = 30;
inline constexpr uint8_t kStartScenarioReq = 31;
inline constexpr uint8_t kStartScenarioResp = 32;
inline constexpr uint8_t kQuestBookOpen = 33;
// Server-driven recipe queries (client↔gateway↔RecipeManagerService).
// Payload is always a Protocol::RecipeFrame (recipe.fbs).
inline constexpr uint8_t kRecipeCheckReq = 34;
inline constexpr uint8_t kRecipeCheckResp = 35;
inline constexpr uint8_t kRecipeCatalogReq = 36;
inline constexpr uint8_t kRecipeCatalogResp = 37;
inline constexpr uint8_t kRecipeItemReq = 38;
inline constexpr uint8_t kRecipeItemResp = 39;
inline constexpr uint8_t kRecipeMachineReq = 40;
inline constexpr uint8_t kRecipeMachineResp = 41;
inline constexpr uint8_t kBlockActionDirective = 42;
inline constexpr uint8_t kGridUpdate = 43;
inline constexpr uint8_t kWorkbenchOpenReq = 44;
// Server-authoritative machine/port buffer state (Protocol::ResourceBufferState,
// client_state.fbs). SimulationCore → gateway → client ctrl connection.
inline constexpr uint8_t kResourceBufferState = 47;
// Client-side historical alias for kEntitySnapshot (NetClient.h).
inline constexpr uint8_t kEntitySnap = kEntitySnapshot;
} // namespace GatewayMsg
