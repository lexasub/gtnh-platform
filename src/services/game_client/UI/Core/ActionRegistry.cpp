#include "Core/ActionRegistry.h"

void ActionRegistry::Register(const std::string& name, Fn fn) {
    actions_[name] = std::move(fn);
}

void ActionRegistry::Unregister(const std::string& name) {
    actions_.erase(name);
}

ActionRegistry::Fn ActionRegistry::Find(const std::string& name) const {
    if (const auto it = actions_.find(name); it != actions_.end())
        return it->second;
    return nullptr;
}
