// ConditionEvaluator unit tests (issue gp-thhw).
//
// Covers src/game/recipes/ConditionEvaluator.cpp — the gate that decides whether
// a recipe may run against a MachineState.
//
// The public surface is exactly one method, `evaluate(recipe, machineState)`.
// Everything else (checkEnvironment / checkMachine / checkSpecial) is private,
// so every clause below is exercised through evaluate() with a Recipe whose
// RecipeConditions carries exactly the clause under test and a MachineState
// built in memory. No filesystem data is loaded.
//
// The three condition families, and their operators as implemented:
//
//   EnvironmentConditions.temperature  TemperatureRange{min,max}, INCLUSIVE on
//                                     both ends (`t < min || t > max` fails).
//   EnvironmentConditions.purity       minimum, inclusive (`purity < min` fails).
//   EnvironmentConditions.biomes       membership in a non-empty allow-list.
//
//   MachineConditions.energy_min       minimum, inclusive (`energy < min` fails).
//   MachineConditions.energy_max       maximum, inclusive (`energy > max` fails).
//   MachineConditions.network_id       membership in state.network_ids.
//   MachineConditions.facing           exact equality.
//
//   std::vector<SpecialCondition>       typed key/value tags; EVERY recipe tag
//                                      must be present on the machine with an
//                                      identical value_type and value (int exact,
//                                      float within 0.001f absolute, string exact).
//                                      An unrecognised value_type never matches.
//
// Every one of those fields is a std::optional / vector, so a *present but
// empty* clause is not a constraint: an EnvironmentConditions{} with no
// temperature, no purity and no biomes accepts every state. The tests pin that
// explicitly because it is the easiest thing to get wrong when reading YAML
// into these structs.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h).
// GTest is deliberately not used — it is not a dependency of this project.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ConditionEvaluator.h"
#include "RecipeConditions.h"
#include "RecipeTypes.h"

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has no
// GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_NEAR
#define CHECK_NEAR(a, b, eps, ...)                                              \
  test_check(std::abs(static_cast<double>(a) - static_cast<double>(b)) <=       \
                 static_cast<double>(eps),                                     \
             __FILE__, __LINE__, #a " ~= " #b, ##__VA_ARGS__)
#endif

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg) {
  if (!cond) {
    fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
    if (msg)
      fprintf(stderr, " -- %s", msg);
    fprintf(stderr, "\n");
    ++g_failed;
  } else {
    ++g_passed;
  }
}

// ---------------------------------------------------------------------------
// Fixture helpers — every state is constructed in memory
// ---------------------------------------------------------------------------

using RecipeManager::ConditionEvaluator;
using RecipeManager::EnvironmentConditions;
using RecipeManager::MachineConditions;
using RecipeManager::MachineState;
using RecipeManager::Recipe;
using RecipeManager::RecipeConditions;
using RecipeManager::SpecialCondition;
using RecipeManager::TemperatureRange;

// A neutral, fully-satisfying machine state. Individual tests move exactly one
// field so a failure always points at the clause under test.
static MachineState makeState() {
  MachineState s{};
  s.temperature = 300.0f;
  s.purity = 1.0f;
  s.energy = 1000;
  s.biome_id = 0;
  s.facing = 0;
  return s;
}

static Recipe makeRecipe() {
  Recipe r{};
  r.conditions = RecipeConditions{};
  return r;
}

static bool evaluateWith(RecipeConditions conds, const MachineState &s) {
  Recipe r = makeRecipe();
  r.conditions = std::move(conds);
  ConditionEvaluator ev;
  return ev.evaluate(r, s);
}

// --- environment -----------------------------------------------------------

static bool evalEnv(const EnvironmentConditions &env, const MachineState &s) {
  RecipeConditions c;
  c.environment = env;
  return evaluateWith(std::move(c), s);
}

static bool evalTemp(float min, float max, float stateTemp) {
  EnvironmentConditions env;
  env.temperature = TemperatureRange{min, max};
  MachineState s = makeState();
  s.temperature = stateTemp;
  return evalEnv(env, s);
}

static bool evalPurity(float required, float statePurity) {
  EnvironmentConditions env;
  env.purity = required;
  MachineState s = makeState();
  s.purity = statePurity;
  return evalEnv(env, s);
}

static bool evalBiome(std::vector<uint16_t> list, uint16_t stateBiome) {
  EnvironmentConditions env;
  env.biomes = std::move(list);
  MachineState s = makeState();
  s.biome_id = stateBiome;
  return evalEnv(env, s);
}

// --- machine ---------------------------------------------------------------

static bool evalMachine(const MachineConditions &m, const MachineState &s) {
  RecipeConditions c;
  c.machine = m;
  return evaluateWith(std::move(c), s);
}

static bool evalEnergy(const std::optional<uint32_t> &emin,
                       const std::optional<uint32_t> &emax, uint32_t stateEnergy) {
  MachineConditions m;
  m.energy_min = emin;
  m.energy_max = emax;
  MachineState s = makeState();
  s.energy = stateEnergy;
  return evalMachine(m, s);
}

static bool evalNetwork(uint32_t netId, std::vector<uint32_t> networks) {
  MachineConditions m;
  m.network_id = netId;
  MachineState s = makeState();
  s.network_ids = std::move(networks);
  return evalMachine(m, s);
}

static bool evalFacing(uint8_t required, uint8_t stateFacing) {
  MachineConditions m;
  m.facing = required;
  MachineState s = makeState();
  s.facing = stateFacing;
  return evalMachine(m, s);
}

// --- special tags ----------------------------------------------------------

static SpecialCondition tagInt(uint16_t key, int32_t value) {
  SpecialCondition t{};
  t.key = key;
  t.value_type = 0; // int32
  t.int_value = value;
  t.float_value = 0.0f;
  t.string_value.clear();
  return t;
}

static SpecialCondition tagFloat(uint16_t key, float value) {
  SpecialCondition t{};
  t.key = key;
  t.value_type = 1; // float
  t.int_value = 0;
  t.float_value = value;
  t.string_value.clear();
  return t;
}

static SpecialCondition tagString(uint16_t key, std::string value) {
  SpecialCondition t{};
  t.key = key;
  t.value_type = 2; // string
  t.int_value = 0;
  t.float_value = 0.0f;
  t.string_value = std::move(value);
  return t;
}

static bool evalSpecial(std::vector<SpecialCondition> recipeTags,
                        std::vector<SpecialCondition> machineTags) {
  RecipeConditions c;
  c.special = std::move(recipeTags);
  MachineState s = makeState();
  s.tags = std::move(machineTags);
  return evaluateWith(std::move(c), s);
}

// ---------------------------------------------------------------------------
// Top level: clause presence and conjunction
// ---------------------------------------------------------------------------

static void test_no_conditions_is_always_satisfied() {
  MachineState s = makeState();
  s.temperature = -273.15f;
  s.purity = 0.0f;
  s.energy = 0;
  s.biome_id = 65535;
  s.facing = 5;
  CHECK(evaluateWith(RecipeConditions{}, s),
        "a recipe with no conditions runs anywhere");
}

static void test_empty_environment_clause_is_a_no_op() {
  EnvironmentConditions env; // no temperature, no purity, no biomes
  MachineState s = makeState();
  s.temperature = -1000.0f;
  s.purity = 0.0f;
  s.biome_id = 1234;
  CHECK(evalEnv(env, s),
        "a present-but-empty environment clause constrains nothing");
}

static void test_empty_machine_clause_is_a_no_op() {
  MachineConditions m; // no energy_min/max, no network_id, no facing
  MachineState s = makeState();
  s.energy = 0;
  s.facing = 3;
  CHECK(evalMachine(m, s),
        "a present-but-empty machine clause constrains nothing");
}

static void test_empty_special_clause_is_a_no_op() {
  // The machine carries tags the recipe never asked for.
  CHECK(evalSpecial({}, {tagInt(1, 5), tagString(2, "hello")}),
        "no recipe tags means machine tags are never consulted");
}

static void test_environment_and_machine_are_conjunctive() {
  RecipeConditions c;
  EnvironmentConditions env;
  env.temperature = TemperatureRange{200.0f, 400.0f};
  c.environment = env;
  MachineConditions m;
  m.energy_min = 100;
  c.machine = m;

  MachineState ok = makeState();
  ok.temperature = 300.0f;
  ok.energy = 100;
  CHECK(evaluateWith(c, ok), "both clauses satisfied -> runnable");

  MachineState badEnv = ok;
  badEnv.temperature = 100.0f;
  CHECK(!evaluateWith(c, badEnv), "environment alone can veto");

  MachineState badMachine = ok;
  badMachine.energy = 99;
  CHECK(!evaluateWith(c, badMachine), "machine alone can veto");
}

static void test_all_three_clause_kinds_are_conjunctive() {
  RecipeConditions c;
  EnvironmentConditions env;
  env.biomes = {2, 3};
  c.environment = env;
  MachineConditions m;
  m.facing = 1;
  c.machine = m;
  c.special = {tagInt(7, 42)};

  MachineState s = makeState();
  s.biome_id = 2;
  s.facing = 1;
  s.tags = {tagInt(7, 42)};

  CHECK(evaluateWith(c, s), "all three clause families satisfied");

  MachineState missingTag = s;
  missingTag.tags.clear();
  CHECK(!evaluateWith(c, missingTag), "special clause still vetoes");
}

// ---------------------------------------------------------------------------
// Environment: temperature range (inclusive min and max)
// ---------------------------------------------------------------------------

static void test_temperature_below_min_fails() {
  CHECK(!evalTemp(300.0f, 400.0f, 299.9f), "just below min is rejected");
}

static void test_temperature_at_min_passes() {
  CHECK(evalTemp(300.0f, 400.0f, 300.0f), "min is inclusive");
}

static void test_temperature_mid_range_passes() {
  CHECK(evalTemp(300.0f, 400.0f, 350.0f), "inside the range is accepted");
}

static void test_temperature_at_max_passes() {
  CHECK(evalTemp(300.0f, 400.0f, 400.0f), "max is inclusive");
}

static void test_temperature_above_max_fails() {
  CHECK(!evalTemp(300.0f, 400.0f, 400.1f), "just above max is rejected");
}

static void test_temperature_supports_negative_celsius() {
  CHECK(evalTemp(-40.0f, -10.0f, -20.0f), "negative ranges compare correctly");
  CHECK(!evalTemp(-40.0f, -10.0f, -9.9f), "above a negative max is rejected");
  CHECK(!evalTemp(-40.0f, -10.0f, -40.1f), "below a negative min is rejected");
}

static void test_inverted_temperature_range_matches_nothing() {
  // min > max is degenerate input; the implementation has no validation, and
  // every value fails one of the two comparisons. Pinned so a future range
  // swap/normalisation has to be a deliberate change.
  CHECK(!evalTemp(400.0f, 300.0f, 350.0f), "mid value cannot satisfy min > max");
  CHECK(!evalTemp(400.0f, 300.0f, 400.0f), "value == min still exceeds max");
  CHECK(!evalTemp(400.0f, 300.0f, 300.0f), "value == max still below min");
}

static void test_nan_temperature_is_rejected() {
  // The gate is `t < min || t > max`, and every comparison against NaN is
  // false, so an unguarded NaN temperature satisfies ANY range — including a
  // zero-width one. An uninitialised or corrupt temperature therefore made the
  // machine unconditionally runnable (gp-paja). A NaN reading is not evidence
  // that the machine is in range, so it is rejected like an out-of-range one.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(!evalTemp(300.0f, 400.0f, nan), "a NaN temperature is rejected");
  CHECK(!evalTemp(0.0f, 0.0f, nan), "a NaN temperature fails a zero-width range");
  // The guard must not spill onto the neighbouring clauses: an in-range
  // temperature on a machine whose OTHER state fields are NaN is a separate
  // question, judged only by that field's own clause.
  CHECK(evalTemp(300.0f, 400.0f, 350.0f), "an ordinary temperature still passes");
}

// ---------------------------------------------------------------------------
// Environment: purity (minimum, inclusive)
// ---------------------------------------------------------------------------

static void test_purity_below_threshold_fails() {
  CHECK(!evalPurity(0.9f, 0.89f), "purity under the minimum is rejected");
}

static void test_purity_at_threshold_passes() {
  CHECK(evalPurity(0.9f, 0.9f), "the purity minimum is inclusive");
}

static void test_purity_above_threshold_passes() {
  CHECK(evalPurity(0.9f, 1.0f), "purity over the minimum is accepted");
}

static void test_purity_threshold_zero_accepts_zero_purity() {
  CHECK(evalPurity(0.0f, 0.0f), "purity 0 satisfies a minimum of 0");
}

static void test_purity_is_not_clamped_to_the_unit_interval() {
  // The struct documents purity as 0.0-1.0, but nothing enforces it and the
  // clause is a pure minimum, so out-of-range values are judged on their face.
  CHECK(evalPurity(1.0f, 5.0f), "purity above 1.0 still clears a 1.0 minimum");
  CHECK(!evalPurity(1.0f, -0.5f), "negative purity does not clear it");
}

static void test_nan_purity_is_rejected() {
  // Same shape as the NaN temperature case: the gate is `purity < min`, which
  // is false for NaN, so NaN satisfied any minimum (gp-paja). Rejected.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(!evalPurity(1.0f, nan), "a NaN purity is rejected");
  CHECK(!evalPurity(0.0f, nan), "a NaN purity fails even a zero minimum");
  // And the well-formed cases are untouched.
  CHECK(evalPurity(1.0f, 1.0f), "a purity equal to the minimum still passes");
}

// ---------------------------------------------------------------------------
// Environment: biome allow-list
// ---------------------------------------------------------------------------

static void test_biome_list_membership_is_required() {
  CHECK(evalBiome({0, 1, 2}, 1), "a listed biome is accepted");
}

static void test_biome_outside_list_fails() {
  CHECK(!evalBiome({0, 1, 2}, 3), "an unlisted biome is rejected");
}

static void test_empty_biome_list_imposes_no_biome_constraint() {
  CHECK(evalBiome({}, 0), "an empty allow-list accepts biome 0");
  CHECK(evalBiome({}, 65535), "an empty allow-list accepts any biome");
}

static void test_biome_list_accepts_any_listed_member() {
  CHECK(evalBiome({10, 20, 30}, 10), "first entry matches");
  CHECK(evalBiome({10, 20, 30}, 20), "middle entry matches");
  CHECK(evalBiome({10, 20, 30}, 30), "last entry matches");
  CHECK(!evalBiome({10, 20, 30}, 31), "one past the list does not match");
}

static void test_biome_ids_span_the_full_uint16_range() {
  CHECK(evalBiome({0}, 0), "biome id 0 is a real id, not a null sentinel");
  CHECK(evalBiome({65535}, 65535), "the top of the uint16 range matches");
  CHECK(!evalBiome({65534}, 65535), "and does not leak into neighbours");
}

// ---------------------------------------------------------------------------
// Machine: energy window (inclusive on both ends)
// ---------------------------------------------------------------------------

static void test_energy_below_min_fails() {
  CHECK(!evalEnergy(1000, std::nullopt, 999), "just below energy_min is rejected");
}

static void test_energy_at_min_passes() {
  CHECK(evalEnergy(1000, std::nullopt, 1000), "energy_min is inclusive");
}

static void test_energy_at_max_passes() {
  CHECK(evalEnergy(std::nullopt, 1000, 1000), "energy_max is inclusive");
}

static void test_energy_above_max_fails() {
  CHECK(!evalEnergy(std::nullopt, 1000, 1001), "just above energy_max is rejected");
}

static void test_energy_mid_window_passes() {
  CHECK(evalEnergy(1000, 2000, 1500), "inside the window is accepted");
  CHECK(!evalEnergy(1000, 2000, 999), "below the window is rejected");
  CHECK(!evalEnergy(1000, 2000, 2001), "above the window is rejected");
}

static void test_energy_min_alone_imposes_no_upper_bound() {
  CHECK(evalEnergy(10, std::nullopt, 4000000000u),
        "with no energy_max any stored charge satisfies the clause");
}

static void test_energy_max_alone_imposes_no_lower_bound() {
  CHECK(evalEnergy(std::nullopt, 10, 0), "with no energy_min zero charge passes");
}

static void test_inverted_energy_window_matches_nothing() {
  CHECK(!evalEnergy(2000, 1000, 1500), "min above max can never be satisfied");
}

static void test_energy_clause_absent_ignores_machine_energy() {
  MachineState s = makeState();
  s.energy = 0;
  CHECK(evaluateWith(RecipeConditions{}, s),
        "with no machine clause, stored energy is irrelevant");
}

// ---------------------------------------------------------------------------
// Machine: network membership
// ---------------------------------------------------------------------------

static void test_connected_network_satisfies_the_clause() {
  CHECK(evalNetwork(7, {7}), "a single attached network matches");
}

static void test_unconnected_network_fails() {
  CHECK(!evalNetwork(7, {1, 2, 3}), "an absent network is rejected");
}

static void test_network_clause_fails_with_no_connections() {
  CHECK(!evalNetwork(7, {}), "no connections cannot satisfy a network_id");
}

static void test_network_membership_is_position_independent() {
  CHECK(evalNetwork(3, {9, 8, 3, 7}), "membership anywhere in the list matches");
  CHECK(evalNetwork(3, {3, 3, 3}), "duplicate network ids still match");
}

static void test_network_id_zero_is_enforced_when_set() {
  // network_id is a std::optional, so 0 is a real network id rather than a
  // "unset" sentinel — and the clause is skipped entirely when unset.
  CHECK(evalNetwork(0, {0}), "network 0 satisfies a network_id of 0");
  CHECK(!evalNetwork(0, {1, 2}), "network 0 does not satisfy a machine without it");

  MachineState s = makeState();
  s.network_ids.clear();
  CHECK(evaluateWith(RecipeConditions{}, s),
        "with no machine clause network ids are ignored");
}

// ---------------------------------------------------------------------------
// Machine: facing
// ---------------------------------------------------------------------------

static void test_matching_facing_passes() {
  CHECK(evalFacing(3, 3), "equal facing passes");
}

static void test_mismatched_facing_fails() {
  CHECK(!evalFacing(3, 4), "any other facing fails");
}

static void test_facing_zero_is_enforced_when_set() {
  CHECK(evalFacing(0, 0), "facing 0 is a real value, not a null sentinel");
  CHECK(!evalFacing(1, 0), "facing 0 does not satisfy a requirement of 1");
}

static void test_facing_is_not_range_validated() {
  // The field is documented as 0-5, but the clause is plain equality and the
  // block meta is not validated here, so 6 only matches 6.
  CHECK(evalFacing(6, 6), "a documented-illegal facing still matches itself");
  CHECK(!evalFacing(6, 5), "and does not match a legal one");
}

// ---------------------------------------------------------------------------
// Special tags: int32 (value_type 0)
// ---------------------------------------------------------------------------

static void test_matching_int_tag_passes() {
  CHECK(evalSpecial({tagInt(1, 42)}, {tagInt(1, 42)}), "equal int tags match");
}

static void test_mismatched_int_tag_fails() {
  CHECK(!evalSpecial({tagInt(1, 42)}, {tagInt(1, 43)}), "differing ints fail");
  CHECK(!evalSpecial({tagInt(1, 42)}, {tagInt(1, -42)}), "sign matters");
}

static void test_int_tag_ignores_the_other_value_fields() {
  // Only int_value is read for value_type 0, so junk in the float/string slots
  // on both sides is irrelevant.
  SpecialCondition want = tagInt(1, 5);
  want.float_value = 999.0f;
  want.string_value = "ignored";
  SpecialCondition have = tagInt(1, 5);
  have.float_value = -42.0f;
  have.string_value = "different";
  CHECK(evalSpecial({want}, {have}), "int comparison reads int_value only");
}

static void test_multiple_required_tags_must_all_match() {
  std::vector<SpecialCondition> want = {tagInt(1, 7), tagInt(2, 8), tagString(3, "x")};
  CHECK(evalSpecial(want, {tagInt(1, 7), tagInt(2, 8), tagString(3, "x")}),
        "all three tags present and equal -> runnable");
  CHECK(!evalSpecial(want, {tagInt(1, 7), tagInt(2, 8)}),
        "one missing tag is enough to veto");
  CHECK(!evalSpecial(want, {tagInt(1, 7), tagInt(2, 99), tagString(3, "x")}),
        "one wrong value is enough to veto");
}

static void test_extra_machine_tags_are_allowed() {
  CHECK(evalSpecial({tagInt(1, 7)}, {tagInt(1, 7), tagInt(2, 8), tagString(3, "x")}),
        "the machine may carry more tags than the recipe requires");
}

static void test_missing_tag_key_fails() {
  CHECK(!evalSpecial({tagInt(1, 7)}, {tagInt(2, 7)}),
        "a machine tag with a different key does not stand in");
  CHECK(!evalSpecial({tagInt(1, 7)}, {}), "an untagged machine satisfies nothing");
}

// ---------------------------------------------------------------------------
// Special tags: float (value_type 1) and the 0.001f absolute tolerance
// ---------------------------------------------------------------------------

static void test_matching_float_tag_passes() {
  CHECK(evalSpecial({tagFloat(1, 1.5f)}, {tagFloat(1, 1.5f)}), "equal floats match");
}

static void test_float_tag_within_tolerance_passes() {
  // Exactly 0.001f apart, computed from literals that are bit-identical after
  // subtraction, so the boundary is exact rather than representation-dependent.
  CHECK(evalSpecial({tagFloat(1, 0.0f)}, {tagFloat(1, 0.001f)}),
        "a difference of exactly the tolerance still matches (abs > eps fails)");
  CHECK(evalSpecial({tagFloat(1, 0.5f)}, {tagFloat(1, 0.5f)}), "zero difference matches");
}

static void test_float_tag_beyond_tolerance_fails() {
  CHECK(!evalSpecial({tagFloat(1, 0.0f)}, {tagFloat(1, 0.0015f)}),
        "a difference past the tolerance fails");
  CHECK(!evalSpecial({tagFloat(1, 1.0f)}, {tagFloat(1, 2.0f)}),
        "a wildly different float fails");
}

static void test_float_tag_comparison_is_sign_agnostic() {
  // std::abs is applied, so the sign of the difference is irrelevant.
  CHECK(evalSpecial({tagFloat(1, 0.0f)}, {tagFloat(1, -0.001f)}),
        "-0.001f is within tolerance of 0.0f");
  CHECK(!evalSpecial({tagFloat(1, 0.0f)}, {tagFloat(1, -0.002f)}),
        "-0.002f is outside it");
}

static void test_nan_float_tag_matches_nothing() {
  // The tolerance test is `abs(a - b) > 0.001f`, which is false for a NaN
  // difference, so a NaN on EITHER side used to match any float tag (gp-paja):
  // a recipe demanding fluid X was satisfied by a machine reporting NaN. A NaN
  // on either side is now a non-match, because "unmeasurable" is not "equal".
  // Infinity is not NaN and was already correctly rejected against a finite tag.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  CHECK(!evalSpecial({tagFloat(1, nan)}, {tagFloat(1, 1.5f)}),
        "a NaN recipe float matches no machine float");
  CHECK(!evalSpecial({tagFloat(1, 1.5f)}, {tagFloat(1, nan)}),
        "a NaN machine float matches no recipe float");
  CHECK(!evalSpecial({tagFloat(1, nan)}, {tagFloat(1, nan)}),
        "NaN never matches NaN either");
  CHECK(!evalSpecial({tagFloat(1, inf)}, {tagFloat(1, 1.5f)}),
        "infinity is still rejected against a finite tag");
  // The tolerance itself is unchanged for finite values.
  CHECK(evalSpecial({tagFloat(1, 1.0f)}, {tagFloat(1, 1.0005f)}),
        "two finite floats within tolerance still match");
}

// ---------------------------------------------------------------------------
// Special tags: string (value_type 2)
// ---------------------------------------------------------------------------

static void test_matching_string_tag_passes() {
  CHECK(evalSpecial({tagString(1, "plasma")}, {tagString(1, "plasma")}),
        "equal strings match");
}

static void test_mismatched_string_tag_fails() {
  CHECK(!evalSpecial({tagString(1, "plasma")}, {tagString(1, "water")}),
        "differing strings fail");
  CHECK(!evalSpecial({tagString(1, "plasma")}, {tagString(1, "")}),
        "an empty machine string is not a wildcard");
  CHECK(!evalSpecial({tagString(1, "")}, {tagString(1, "plasma")}),
        "an empty recipe string is not a wildcard either");
}

static void test_string_tag_comparison_is_case_sensitive() {
  CHECK(!evalSpecial({tagString(1, "Plasma")}, {tagString(1, "plasma")}),
        "comparison is case sensitive");
}

// ---------------------------------------------------------------------------
// Special tags: type discipline and unknown types
// ---------------------------------------------------------------------------

static void test_type_mismatch_fails_even_when_values_agree() {
  // 42 and 42.0f are numerically equal, but the value_type gate rejects the
  // pair before any value comparison happens.
  CHECK(!evalSpecial({tagInt(1, 42)}, {tagFloat(1, 42.0f)}),
        "int recipe tag does not match a float machine tag");
  CHECK(!evalSpecial({tagString(1, "42")}, {tagInt(1, 42)}),
        "string recipe tag does not match an int machine tag");
}

static void test_unknown_value_type_fails() {
  SpecialCondition recipe3 = tagInt(1, 1);
  recipe3.value_type = 3;
  SpecialCondition machine3 = tagInt(1, 1);
  machine3.value_type = 3;
  CHECK(!evalSpecial({recipe3}, {machine3}),
        "an unrecognised value_type never matches, even against itself");

  SpecialCondition recipe255 = tagInt(1, 1);
  recipe255.value_type = 255;
  SpecialCondition machine255 = tagInt(1, 1);
  machine255.value_type = 255;
  CHECK(!evalSpecial({recipe255}, {machine255}),
        "255 (the ENERGY_TYPE_ANY sentinel value) is not a valid tag type");
}

static void test_duplicate_machine_key_resolves_to_the_last_tag() {
  // checkSpecial builds a key -> tag map, so a repeated key overwrites the
  // earlier entry rather than being compared against it.
  std::vector<SpecialCondition> machine = {tagInt(5, 1), tagInt(5, 2)};
  CHECK(evalSpecial({tagInt(5, 2)}, machine), "the last tag for a key wins");
  CHECK(!evalSpecial({tagInt(5, 1)}, machine), "the shadowed tag does not match");
}

static void test_duplicate_recipe_keys_are_each_checked() {
  CHECK(evalSpecial({tagInt(1, 7), tagInt(1, 7)}, {tagInt(1, 7)}),
        "a repeated identical requirement is satisfied by one tag");
  CHECK(!evalSpecial({tagInt(1, 7), tagInt(1, 8)}, {tagInt(1, 7)}),
        "a repeated contradictory requirement cannot be satisfied");
}

static void test_tag_key_zero_is_matched() {
  CHECK(evalSpecial({tagInt(0, 1)}, {tagInt(0, 1)}), "key 0 is a real tag key");
  CHECK(!evalSpecial({tagInt(0, 1)}, {tagInt(1, 1)}), "key 0 is not a wildcard");
}

static void test_tag_key_top_of_range_is_matched() {
  CHECK(evalSpecial({tagString(65535, "x")}, {tagString(65535, "x")}),
        "key 65535 is a real tag key");
  CHECK(!evalSpecial({tagString(65535, "x")}, {tagString(65534, "x")}),
        "and does not leak into its neighbour");
}

// ---------------------------------------------------------------------------
// The machine's own tags are what get compared (direction of the check)
// ---------------------------------------------------------------------------

static void test_machine_tags_are_the_comparison_source() {
  // Asymmetric by construction: the machine carries the recipe's tag, and the
  // recipe carries one the machine does not have. The second must veto.
  CHECK(evalSpecial({tagInt(1, 7)}, {tagInt(1, 7), tagInt(2, 8)}),
        "recipe requirements are a subset test against machine tags");
  CHECK(!evalSpecial({tagInt(1, 7), tagInt(3, 9)}, {tagInt(1, 7), tagInt(2, 8)}),
        "a requirement absent from the machine vetoes the recipe");
}

#define TEST(name)                                                             \
  do {                                                                         \
    ++g_tests;                                                                 \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("=== condition_evaluator test suite ===\n\n");

  TEST(no_conditions_is_always_satisfied);
  TEST(empty_environment_clause_is_a_no_op);
  TEST(empty_machine_clause_is_a_no_op);
  TEST(empty_special_clause_is_a_no_op);
  TEST(environment_and_machine_are_conjunctive);
  TEST(all_three_clause_kinds_are_conjunctive);

  TEST(temperature_below_min_fails);
  TEST(temperature_at_min_passes);
  TEST(temperature_mid_range_passes);
  TEST(temperature_at_max_passes);
  TEST(temperature_above_max_fails);
  TEST(temperature_supports_negative_celsius);
  TEST(inverted_temperature_range_matches_nothing);
  TEST(nan_temperature_is_rejected);

  TEST(purity_below_threshold_fails);
  TEST(purity_at_threshold_passes);
  TEST(purity_above_threshold_passes);
  TEST(purity_threshold_zero_accepts_zero_purity);
  TEST(purity_is_not_clamped_to_the_unit_interval);
  TEST(nan_purity_is_rejected);

  TEST(biome_list_membership_is_required);
  TEST(biome_outside_list_fails);
  TEST(empty_biome_list_imposes_no_biome_constraint);
  TEST(biome_list_accepts_any_listed_member);
  TEST(biome_ids_span_the_full_uint16_range);

  TEST(energy_below_min_fails);
  TEST(energy_at_min_passes);
  TEST(energy_at_max_passes);
  TEST(energy_above_max_fails);
  TEST(energy_mid_window_passes);
  TEST(energy_min_alone_imposes_no_upper_bound);
  TEST(energy_max_alone_imposes_no_lower_bound);
  TEST(inverted_energy_window_matches_nothing);
  TEST(energy_clause_absent_ignores_machine_energy);

  TEST(connected_network_satisfies_the_clause);
  TEST(unconnected_network_fails);
  TEST(network_clause_fails_with_no_connections);
  TEST(network_membership_is_position_independent);
  TEST(network_id_zero_is_enforced_when_set);

  TEST(matching_facing_passes);
  TEST(mismatched_facing_fails);
  TEST(facing_zero_is_enforced_when_set);
  TEST(facing_is_not_range_validated);

  TEST(matching_int_tag_passes);
  TEST(mismatched_int_tag_fails);
  TEST(int_tag_ignores_the_other_value_fields);
  TEST(multiple_required_tags_must_all_match);
  TEST(extra_machine_tags_are_allowed);
  TEST(missing_tag_key_fails);

  TEST(matching_float_tag_passes);
  TEST(float_tag_within_tolerance_passes);
  TEST(float_tag_beyond_tolerance_fails);
  TEST(float_tag_comparison_is_sign_agnostic);
  TEST(nan_float_tag_matches_nothing);

  TEST(matching_string_tag_passes);
  TEST(mismatched_string_tag_fails);
  TEST(string_tag_comparison_is_case_sensitive);

  TEST(type_mismatch_fails_even_when_values_agree);
  TEST(unknown_value_type_fails);
  TEST(duplicate_machine_key_resolves_to_the_last_tag);
  TEST(duplicate_recipe_keys_are_each_checked);
  TEST(tag_key_zero_is_matched);
  TEST(tag_key_top_of_range_is_matched);

  TEST(machine_tags_are_the_comparison_source);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
