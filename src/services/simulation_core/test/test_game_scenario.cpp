// GameScenario / StartScenario flow tests:
// 7.1 protocol frame round-trip; 7.2 scenario table + apply ordering.
#include <libgtnh-net/test/test.h>

#include "Scenario/GameScenario.h"
#include "Storage/PlayerInventoryStore.h"
#include "core_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <cstdint>
#include <array>
#include <vector>

#ifndef TEST
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)
#endif

static void test_StartScenarioReqFrame() {
  flatbuffers::FlatBufferBuilder fbb(64);
  auto req = Protocol::CreateStartScenarioReq(fbb, 1234, 0);
  fbb.Finish(req);

  flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());
  CHECK(v.VerifyBuffer<Protocol::StartScenarioReq>(nullptr));
  auto* parsed =
      flatbuffers::GetRoot<Protocol::StartScenarioReq>(fbb.GetBufferPointer());
  CHECK(parsed != nullptr);
  CHECK_EQ(parsed->player_id(), uint64_t(1234));
  CHECK_EQ(parsed->scenario_index(), uint8_t(0));
}

static void test_StartScenarioRespFrame() {
  flatbuffers::FlatBufferBuilder fbb(64);
  auto err = fbb.CreateString("boom");
  auto resp = Protocol::CreateStartScenarioResp(
      fbb, 1234, 0, false, err, Protocol::GameMode_ADVENTURE, 2);
  fbb.Finish(resp);

  flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());
  CHECK(v.VerifyBuffer<Protocol::StartScenarioResp>(nullptr));
  auto* parsed =
      flatbuffers::GetRoot<Protocol::StartScenarioResp>(fbb.GetBufferPointer());
  CHECK_EQ(parsed->player_id(), uint64_t(1234));
  CHECK_EQ(parsed->scenario_index(), uint8_t(0));
  CHECK_EQ(parsed->success(), false);
  CHECK_EQ(parsed->game_mode(), Protocol::GameMode_ADVENTURE);
  CHECK_EQ(parsed->quest_book_era(), uint8_t(2));
  CHECK(parsed->error() && parsed->error()->str() == "boom");
}

static void test_Scenario0Table() {
  const simcore::GameScenario* sc = simcore::findScenario(0);
  CHECK(sc != nullptr);
  CHECK_EQ(sc->targetMode, uint8_t(0));  // SURVIVAL
  CHECK(sc->clearFirst);
  CHECK_EQ(sc->questBookEra, uint8_t(0));
  CHECK_EQ(sc->giveItems.size(), size_t(2));
  if (sc && sc->giveItems.size() == 2) {
    CHECK_EQ(sc->giveItems[0].first, uint16_t(22529));  // crafting table
    CHECK_EQ(sc->giveItems[1].first, uint16_t(30723));  // wooden pickaxe
  }
}

static void test_ScenarioUnknownRejected() {
  CHECK(simcore::findScenario(1) == nullptr);
  CHECK(simcore::findScenario(255) == nullptr);
}

static void test_ApplyScenarioOrdering() {
  simcore::PlayerInventoryStore store;
  int postMutations = 0;
  store.setPostMutation(
      [&postMutations](uint64_t, const std::array<simcore::PersistSlot, simcore::kInventorySlots>&) {
        ++postMutations;
      });

  store.initPlayer(7);
  std::array<simcore::PersistSlot, simcore::kInventorySlots> prefilled{};
  prefilled[0].item_id = 999;
  prefilled[0].count = 1;
  store.setSlots(7, prefilled);

  const simcore::GameScenario* sc = simcore::findScenario(0);
  CHECK(sc != nullptr);
  if (!sc) return;

  int before = postMutations;
  CHECK(simcore::applyScenario(store, *sc, 7));
  CHECK_EQ(postMutations, before + 3);

  auto slotsAfter = store.getSlots(7);
  bool hasCraftTable = false, hasPickaxe = false, cleared = true;
  for (const auto& s : slotsAfter) {
    if (s.item_id == 22529 && s.count >= 1) hasCraftTable = true;
    if (s.item_id == 30723 && s.count >= 1) hasPickaxe = true;
    if (s.item_id == 999) cleared = false;  // pre-grant should be wiped
  }
  CHECK(hasCraftTable);
  CHECK(hasPickaxe);
  CHECK(cleared);
  CHECK_EQ(store.getGameMode(7), uint8_t(0));  // SURVIVAL
}

void test_game_scenario() {
  TEST(StartScenarioReqFrame);
  TEST(StartScenarioRespFrame);
  TEST(Scenario0Table);
  TEST(ScenarioUnknownRejected);
  TEST(ApplyScenarioOrdering);
}
