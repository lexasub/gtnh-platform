#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

enum class EnergyType : uint8_t {
  ELECTRICITY = 0,
  HEAT = 1,
  STEAM = 2,
  ROTATION = 3,
};

enum class MachineRole : uint8_t {
  CONSUMER = 0,
  PRODUCER = 1,
};

struct MachineInfo {
  uint16_t id;
  std::string name;
  std::string machine_class;
  MachineRole role;
  // CONSUMER: всегда set; PRODUCER: опц. (только для гибридов вроде
  // steam_heat_boiler)
  std::optional<EnergyType> energy_in;
  std::optional<EnergyType> energy_out;
  int tier;
  int slots_in;
  int slots_out;
  int capacity;
  int maxInput;
  int maxOutput;
};

class MachineRegistry {
public:
  static std::unique_ptr<MachineRegistry> Load(const char *consumers_path,
                                               const char *producers_path);

  /// Load machine definitions from machines.yaml (replaces CSV).
  /// Call this instead of Load() to use the new YAML registry.
  static std::unique_ptr<MachineRegistry> LoadFromYaml(const char *yaml_path);

  // Global singleton access (set by whoever loads the registry)
  static MachineRegistry *instance() { return instance_; }
  static void setInstance(MachineRegistry *reg) { instance_ = reg; }

  const MachineInfo *Get(uint16_t block_id) const;
  bool IsMachine(uint16_t block_id) const;
  bool IsConsumer(uint16_t block_id) const;
  bool IsProducer(uint16_t block_id) const;
  const std::unordered_map<uint16_t, MachineInfo> &All() const;
  static const char *EnergyLabel(EnergyType et);
  static const char *EnergyTypeToString(EnergyType et);

private:
  MachineRegistry() = default;
  bool LoadConsumers(const char *path);
  bool LoadProducers(const char *path);
  bool ParseYamlMachineVariant(const YAML::Node &variant,
                               const std::string &className);
  std::unordered_map<uint16_t, MachineInfo> machines_;

  static MachineRegistry *instance_;
};
