#pragma once

#include <functional>
#include <string>
#include <unordered_map>

class ActionRegistry {
public:
    using Fn = std::function<void()>;

    void Register(const std::string& name, Fn fn);
    void Unregister(const std::string& name);
    Fn Find(const std::string& name) const;

private:
    std::unordered_map<std::string, Fn> actions_;
};
