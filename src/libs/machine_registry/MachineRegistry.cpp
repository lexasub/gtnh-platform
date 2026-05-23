#include "MachineRegistry.h"
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

namespace {

inline EnergyType ParseEnergy(const std::string& str) {
    if (str == "ELECTRICITY") return EnergyType::ELECTRICITY;
    if (str == "HEAT")        return EnergyType::HEAT;
    if (str == "STEAM")       return EnergyType::STEAM;
    return EnergyType::ELECTRICITY;
}

inline MachineRole ParseRole(const std::string& str) {
    if (str == "CONSUMER")    return MachineRole::CONSUMER;
    if (str == "PRODUCER")    return MachineRole::PRODUCER;
    return MachineRole::CONSUMER;
}

inline bool ParseOptionalEnergy(const std::string& str,
                                std::optional<EnergyType>& out) {
    if (str.empty()) {
        out = std::nullopt;
        return true;
    }
    out = ParseEnergy(str);
    return true;
}

inline std::vector<std::string> SplitCSVLine(const std::string& line) {
    std::vector<std::string> result;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        result.push_back(token);
    }
    return result;
}

} // anonymous namespace

std::unique_ptr<MachineRegistry> MachineRegistry::Load(const char* consumers_path,
                                                        const char* producers_path) {
    auto registry = std::unique_ptr<MachineRegistry>(new MachineRegistry());
    if (!registry->LoadConsumers(consumers_path)) {
        SPDLOG_WARN("Failed to load consumers from {}", consumers_path);
    }
    if (!registry->LoadProducers(producers_path)) {
        SPDLOG_WARN("Failed to load producers from {}", producers_path);
    }
    return registry;
}

bool MachineRegistry::LoadConsumers(const char* path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SPDLOG_ERROR("Cannot open consumers file: {}", path);
        return false;
    }

    std::string line;
    // Skip header
    if (!std::getline(file, line)) return false;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto fields = SplitCSVLine(line);
        if (fields.size() < 10) {
            SPDLOG_WARN("Invalid consumer line (too few fields): {}", line);
            continue;
        }

        try {
            uint16_t id = std::stoi(fields[0]);
            std::string name = fields[1];
            std::string machine_class = fields[2];
            EnergyType energy_in = ParseEnergy(fields[3]);
            int tier = std::stoi(fields[4]);
            int slots_in = std::stoi(fields[5]);
            int slots_out = std::stoi(fields[6]);
            int capacity = std::stoi(fields[7]);
            int maxInput = std::stoi(fields[8]);
            int maxOutput = std::stoi(fields[9]);

            MachineInfo info;
            info.id = id;
            info.name = name;
            info.machine_class = machine_class;
            info.role = MachineRole::CONSUMER;
            info.energy_in = energy_in;
            info.energy_out = std::nullopt;
            info.tier = tier;
            info.slots_in = slots_in;
            info.slots_out = slots_out;
            info.capacity = capacity;
            info.maxInput = maxInput;
            info.maxOutput = maxOutput;

            machines_.insert({id, std::move(info)});
        } catch (const std::exception& e) {
            SPDLOG_WARN("Parse error in consumer line '{}': {}", line, e.what());
            continue;
        }
    }
    SPDLOG_DEBUG("Loaded {} consumers", machines_.size());
    return true;
}

bool MachineRegistry::LoadProducers(const char* path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SPDLOG_ERROR("Cannot open producers file: {}", path);
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) return false;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto fields = SplitCSVLine(line);
        if (fields.size() < 11) {
            SPDLOG_WARN("Invalid producer line (too few fields): {}", line);
            continue;
        }

        try {
            uint16_t id = std::stoi(fields[0]);
            std::string name = fields[1];
            std::string machine_class = fields[2];
            std::optional<EnergyType> energy_out;
            ParseOptionalEnergy(fields[3], energy_out);
            std::optional<EnergyType> energy_in;
            ParseOptionalEnergy(fields[4], energy_in);
            int tier = std::stoi(fields[5]);
            int slots_in = std::stoi(fields[6]);
            int slots_out = std::stoi(fields[7]);
            int capacity = std::stoi(fields[8]);
            int maxInput = std::stoi(fields[9]);
            int maxOutput = std::stoi(fields[10]);

            MachineInfo info;
            info.id = id;
            info.name = name;
            info.machine_class = machine_class;
            info.role = MachineRole::PRODUCER;
            info.energy_in = energy_in;
            info.energy_out = energy_out;
            info.tier = tier;
            info.slots_in = slots_in;
            info.slots_out = slots_out;
            info.capacity = capacity;
            info.maxInput = maxInput;
            info.maxOutput = maxOutput;

            machines_.insert({id, std::move(info)});
        } catch (const std::exception& e) {
            SPDLOG_WARN("Parse error in producer line '{}': {}", line, e.what());
            continue;
        }
    }
    SPDLOG_DEBUG("Loaded {} producers", machines_.size());
    return true;
}

const MachineInfo* MachineRegistry::Get(uint16_t block_id) const {
    auto it = machines_.find(block_id);
    return it != machines_.end() ? &it->second : nullptr;
}

bool MachineRegistry::IsMachine(uint16_t block_id) const {
    return machines_.find(block_id) != machines_.end();
}

bool MachineRegistry::IsConsumer(uint16_t block_id) const {
    auto it = machines_.find(block_id);
    return it != machines_.end() && it->second.role == MachineRole::CONSUMER;
}

bool MachineRegistry::IsProducer(uint16_t block_id) const {
    auto it = machines_.find(block_id);
    return it != machines_.end() && it->second.role == MachineRole::PRODUCER;
}

const std::unordered_map<uint16_t, MachineInfo>& MachineRegistry::All() const {
    return machines_;
}

const char* MachineRegistry::EnergyLabel(EnergyType et) {
    switch (et) {
        case EnergyType::ELECTRICITY: return "EU";
        case EnergyType::HEAT:        return "HU";
        case EnergyType::STEAM:       return "SU";
    }
    return "UNKNOWN";
}

const char* MachineRegistry::EnergyTypeToString(EnergyType et) {
    switch (et) {
        case EnergyType::ELECTRICITY: return "ELECTRICITY";
        case EnergyType::HEAT:        return "HEAT";
        case EnergyType::STEAM:       return "STEAM";
    }
    return "UNKNOWN";
}

// static
MachineRegistry* MachineRegistry::instance_ = nullptr;
