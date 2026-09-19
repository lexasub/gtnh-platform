#!/usr/bin/env bash
# Stage 11 first pass: create layer dirs, git mv files, per design:
# openspec/changes/refactor-engine-layer-separation/design.md
# Classes: MOVE whole dirs/files; EXTRACT and KEEP handled in later commits.
set -euo pipefail

run_git() { git "$@"; }

require_clean() {
  if [[ -n "$(git status --porcelain -- src data src/common 2>/dev/null || true)" ]]; then
    echo "ERROR: uncommitted changes in tracked source/data paths. Commit or stash first." >&2
    exit 1
  fi
}

mkdir_layers() {
  mkdir -p src/engine/registry src/engine/sim src/engine/net src/engine/storage
  mkdir -p src/game/machines src/game/recipes src/game/quests src/game/mining
  mkdir -p src/content/data src/apps
}

# MOVE whole-file units only. Include rewrites and CMake edits happen later.
main() {
  require_git_repo
  mkdir_layers

  # engine/registry: src/common -> src/engine/registry
  run_git mv src/common/ItemId.h            src/engine/registry/
  run_git mv src/common/Registry.h          src/engine/registry/
  run_git mv src/common/Registry.cpp        src/engine/registry/
  run_git mv src/common/coords              src/engine/registry/coords
  run_git mv src/common/OpenHashMap.h       src/engine/registry/
  run_git mv src/common/test                src/engine/registry/test
  run_git mv src/common/CMakeLists.txt      src/engine/registry/CMakeLists.txt

  # engine/net: libgtnh-net
  run_git mv src/libs/libgtnh-net           src/engine/net

  # engine/storage: storage_interfaces headers
  run_git mv src/services/storage_interfaces/IEntityStateStorage.h  src/engine/storage/
  run_git mv src/services/storage_interfaces/IPlayerInventoryStorage.h src/engine/storage/
  rmdir src/services/storage_interfaces 2>/dev/null || true

  # game/recipes
  run_git mv src/libs/recipe_manager_lib    src/game/recipes

  # game/quests: quest_lib + QuestManager
  run_git mv src/libs/quest_lib             src/game/quests/lib
  run_git mv src/services/simulation_core/Quest/QuestManager.cpp src/game/quests/
  run_git mv src/services/simulation_core/Quest/QuestManager.h   src/game/quests/
  rmdir src/services/simulation_core/Quest 2>/dev/null || true

  # game/machines: machine_registry + machine ECS systems
  run_git mv src/libs/machine_registry/MachineRegistry.cpp src/game/machines/
  run_git mv src/libs/machine_registry/MachineRegistry.h   src/game/machines/
  rmdir src/libs/machine_registry 2>/dev/null || true
  for f in MachineSystem BoilerSystem GeneratorSystem TransformerSystem SteamTurbineSystem RotareGeneratorSystem LargeBoilerSystem LCRSystem EBFSystem ExplosionSystem CoolantSystem BatteryBufferSystem; do
    run_git mv "src/services/simulation_core/ECS/Systems/${f}.cpp" "src/game/machines/" 2>/dev/null || true
    run_git mv "src/services/simulation_core/ECS/Systems/${f}.h"   "src/game/machines/" 2>/dev/null || true
  done
  run_git mv src/services/simulation_core/ECS/Systems/BoilerPorts.h     src/game/machines/
  run_git mv src/services/simulation_core/ECS/Systems/HeatConstants.h   src/game/machines/

  # game/mining: drill + adjacency + creative generator
  run_git mv src/services/simulation_core/ECS/Systems/DrillSystem.cpp src/game/mining/
  run_git mv src/services/simulation_core/ECS/Systems/DrillSystem.h   src/game/mining/
  run_git mv src/services/simulation_core/ECS/Systems/BatteryBufferSystem.cpp src/game/mining/ 2>/dev/null || true
  run_git mv src/services/simulation_core/ECS/Systems/BatteryBufferSystem.h   src/game/mining/ 2>/dev/null || true
  run_git mv src/services/simulation_core/ECS/Systems/CreativeGeneratorSystem.cpp src/game/mining/
  run_git mv src/services/simulation_core/ECS/Systems/CreativeGeneratorSystem.h   src/game/mining/
  run_git mv src/services/simulation_core/ECS/Systems/AdjacencyTransferSystem.cpp src/game/mining/
  run_git mv src/services/simulation_core/ECS/Systems/AdjacencyTransferSystem.h   src/game/mining/

  # content: data files
  # content: data files (already moved to src/content/data in previous refactor)
  # run_git mv data/bindings.json src/content/data/
  # run_git mv data/quests        src/content/data/quests
  # run_git mv data/recipes       src/content/data/recipes
  # run_git mv data/registry      src/content/data/registry
  # run_git mv data/textures      src/content/data/textures

  # apps: simcore daemon keeps its directory name, moved under apps/
  # apps: simcore daemon already moved to apps/
  # run_git mv src/services/simulation_core src/apps/simcore

  echo "First pass done. Now: fix includes, CMake, then build."
}

require_git_repo() {
  run_git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "ERROR: not a git repo" >&2; exit 1; }
}

main "$@"
