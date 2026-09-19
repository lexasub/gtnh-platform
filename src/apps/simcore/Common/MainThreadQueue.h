#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace simcore {

/// Thread-safe MPSC queue for posting work to the main thread.
/// Multiple producers (poll threads, asio callbacks) push work;
/// single consumer (main thread) drains via drain().
class MainThreadQueue {
public:
    /// Push a callable to be executed on the main thread.
    /// Thread-safe — can be called from any thread.
    template <typename Fn>
    void push(Fn&& fn) {
        std::lock_guard lock(mutex_);
        queue_.emplace_back(std::forward<Fn>(fn));
    }

    /// Drain all queued work on the calling (main) thread.
    /// Executes in FIFO order. Safe to call when queue is empty (no-op).
    void drain() {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard lock(mutex_);
            local.swap(queue_);
        }
        for (auto& fn : local) {
            fn();
        }
    }

    /// Number of pending actions (for diagnostics).
    size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

} // namespace simcore
