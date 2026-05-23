#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct InputState;
class ActionRegistry;

class InputBinder {
public:
    void SetActionRegistry(ActionRegistry* reg) { reg_ = reg; }

    // ── Binding ──────────────────────────────────────────────────────────
    // key = GLFW_KEY_* or GLFW_MOUSE_BUTTON_* (mouse buttons use
    // negative values internally, stored as int).
    // actionName must be registered in ActionRegistry.
    // context = "global" by default.  Only active when context is on stack.
    void Bind(int key, int mods, std::string actionName,
              const char* context = "global");
    void Unbind(int key, int mods, const char* context = "global");
    void UnbindAll(const char* context);

    // Convenience: bind without modifiers
    void Bind(int key, std::string actionName, const char* context = "global") {
        Bind(key, 0, std::move(actionName), context);
    }

    // ── Mouse button convenience ─────────────────────────────────────────
    void BindMouse(int button, int mods, std::string actionName,
                   const char* context = "global");
    void BindMouse(int button, std::string actionName,
                   const char* context = "global") {
        BindMouse(button, 0, std::move(actionName), context);
    }

    // ── Scroll ───────────────────────────────────────────────────────────
    using ScrollFn = std::function<void(float delta)>;
    void OnScroll(ScrollFn fn) { onScroll_ = std::move(fn); }

    // ── Context stack ────────────────────────────────────────────────────
    void PushContext(const char* name);
    void PopContext();
    void RemoveContext(const char* name);
    bool IsContextActive(const char* name) const;
    size_t ActiveContextCount() const { return active_.size(); }

    // ── Config ───────────────────────────────────────────────────────────
    // Load from JSON file: { "bindings": [ { "key":"R", "action":"show_recipe",
    // "mods":0, "ctx":"global" } ] }
    // Called after SetActionRegistry with all actions registered.
    void LoadConfig(const std::string& path);
    void ReloadConfig();

    // ── Per-frame ────────────────────────────────────────────────────────
    void Process(const InputState& cur, const std::array<bool, 512>& prev);

    // ── Edge detection ───────────────────────────────────────────────────
    static bool WasPressed(const std::array<bool, 512>& prev,
                           const InputState& cur, int key);

private:
    struct Binding {
        int key;
        int mods;
        std::string action;
    };
    struct Context {
        std::string name;
        std::vector<Binding> bindings;
    };

    void dispatchBindings(int key, int mods);

    ActionRegistry* reg_ = nullptr;
    std::vector<Context> contexts_;
    std::vector<size_t> active_;       // indices into contexts_, top = last
    ScrollFn onScroll_;
    std::string configPath_;
};
