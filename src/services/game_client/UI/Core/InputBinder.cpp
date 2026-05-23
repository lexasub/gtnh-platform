#include "Core/InputBinder.h"
#include "Core/ActionRegistry.h"
#include "Common/InputState.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void InputBinder::Bind(int key, int mods, std::string actionName,
                        const char* context) {
    auto it = std::ranges::find_if(contexts_,
                                   [&](const Context& c) { return c.name == context; });
    if (it == contexts_.end()) {
        contexts_.push_back({context, {}});
        it = contexts_.end() - 1;
    }
    // Remove existing binding for same key+mods in this context (replace)
    auto& bindings = it->bindings;
    std::erase_if(bindings,
                  [key, mods](const Binding& b) { return b.key == key && b.mods == mods; });
    bindings.push_back({key, mods, std::move(actionName)});
}

void InputBinder::Unbind(int key, int mods, const char* context) {
    for (auto& ctx : contexts_) {
        if (ctx.name == context) {
            auto& b = ctx.bindings;
            std::erase_if(b,
                          [key, mods](const Binding& bind) {
                              return bind.key == key && bind.mods == mods;
                          });
            return;
        }
    }
}

void InputBinder::UnbindAll(const char* context) {
    for (auto&[name, bindings] : contexts_) {
        if (name == context) {
            bindings.clear();
            return;
        }
    }
}

void InputBinder::PushContext(const char* name) {
    for (size_t i = 0; i < contexts_.size(); ++i) {
        if (contexts_[i].name == name) {
            // Don't push duplicates
            if (std::ranges::find(active_.begin(), active_.end(), i) == active_.end())
                active_.push_back(i);
            return;
        }
    }
}

void InputBinder::PopContext() {
    if (!active_.empty())
        active_.pop_back();
}

void InputBinder::RemoveContext(const char* name) {
    std::erase_if(active_,
                  [&](size_t idx) { return contexts_[idx].name == name; });
}

bool InputBinder::IsContextActive(const char* name) const {
    return std::ranges::any_of(active_,
                               [&](size_t idx) { return contexts_[idx].name == name; });
}

void InputBinder::dispatchBindings(int key, int mods) {
    if (!reg_) return;
    // Iterate active contexts top-down (last pushed = highest priority)
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) {
        const auto&[name, bindings] = contexts_[*it];
        for (const auto& b : bindings) {
            if (b.key == key && b.mods == mods) {
                if (auto fn = reg_->Find(b.action)) {
                    fn();
                    return; // consumed
                }
            }
        }
    }
    // Fallback to global context (not pushed explicitly in active_)
    for (const auto& ctx : contexts_) {
        if (ctx.name == "global") {
            for (const auto& b : ctx.bindings) {
                if (b.key == key && b.mods == mods) {
                    if (auto fn = reg_->Find(b.action)) { fn(); return; }
                }
            }
        }
    }
}

bool InputBinder::WasPressed(const std::array<bool, 512>& prev,
                              const InputState& cur, int key) {
    if (key < 0 || key >= 512) return false;
    return cur.keys[key] && !prev[key];
}

void InputBinder::Process(const InputState& cur,
                           const std::array<bool, 512>& prev) {
    // Keyboard keys
    for (const auto& ctx : contexts_) {
        bool ctxActive = (ctx.name == "global") ||
                         std::ranges::find(active_,
                                           &ctx - contexts_.data()) != active_.end();
        if (!ctxActive) continue;
        for (const auto&[key, mods, action] : ctx.bindings) {
            if (key >= 0 && WasPressed(prev, cur, key)) {
                // Check mods
                bool ctrl = cur.keys[GLFW_KEY_LEFT_CONTROL] || cur.keys[GLFW_KEY_RIGHT_CONTROL];
                bool alt  = cur.keys[GLFW_KEY_LEFT_ALT] || cur.keys[GLFW_KEY_RIGHT_ALT];
                bool shift = cur.keys[GLFW_KEY_LEFT_SHIFT] || cur.keys[GLFW_KEY_RIGHT_SHIFT];
                bool modsOk = true;
                if (mods & 1)  modsOk = modsOk && shift;
                if (mods & 2)  modsOk = modsOk && ctrl;
                if (mods & 4)  modsOk = modsOk && alt;
                if (!modsOk) continue;

                if (reg_) {
                    if (auto fn = reg_->Find(action)) { fn(); break; } // consumed for this ctx
                }
            }
        }
    }

    // Scroll
    if (cur.scrollY != 0.0 && onScroll_) {
        onScroll_(static_cast<float>(cur.scrollY));
    }
}

void InputBinder::LoadConfig(const std::string& path) {
    configPath_ = path;
    ReloadConfig();
}

void InputBinder::ReloadConfig() {
    if (configPath_.empty()) return;
    std::ifstream file(configPath_);
    if (!file.is_open()) return;

    try {
        json j;
        file >> j;
        if (!j.contains("bindings")) return;

        for (const auto& entry : j["bindings"]) {
            std::string keyStr = entry.value("key", "");
            std::string action = entry.value("action", "");
            std::string ctx    = entry.value("ctx", "global");
            int mods           = entry.value("mods", 0);

            if (keyStr.empty() || action.empty()) continue;

            // Simple key name → GLFW key mapping
            static const std::unordered_map<std::string, int> s_keyMap = {
                {"Escape", 256}, {"Enter", 257}, {"Tab", 258},
                {"Backspace", 259}, {"Space", 32},
                {"0", 48}, {"1", 49}, {"2", 50}, {"3", 51}, {"4", 52},
                {"5", 53}, {"6", 54}, {"7", 55}, {"8", 56}, {"9", 57},
                {"A", 65}, {"B", 66}, {"C", 67}, {"D", 68}, {"E", 69},
                {"F", 70}, {"G", 71}, {"H", 72}, {"I", 73}, {"J", 74},
                {"K", 75}, {"L", 76}, {"M", 77}, {"N", 78}, {"O", 79},
                {"P", 80}, {"Q", 81}, {"R", 82}, {"S", 83}, {"T", 84},
                {"U", 85}, {"V", 86}, {"W", 87}, {"X", 88}, {"Y", 89},
                {"Z", 90},
            };
            auto it = s_keyMap.find(keyStr);
            if (it == s_keyMap.end()) continue;

            Bind(it->second, mods, action, ctx.c_str());
        }
    } catch (...) {
        // Silently ignore malformed config
    }
}
