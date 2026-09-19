#pragma once

#include <chrono>
#include <string>
#include <atomic>
#include <csignal>
#include <iostream>
#include <cstdlib>
#include <spdlog/spdlog.h>

namespace gtnh {
namespace metrics {

struct UptimeInfo {
    int days;
    int hours;
    int minutes;
    int seconds;
};

inline UptimeInfo calculateUptime(const std::chrono::steady_clock::time_point& start_time) {
    auto now = std::chrono::steady_clock::now();
    auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now - start_time).count();

    return {
        static_cast<int>(uptime_seconds / 86400),
        static_cast<int>((uptime_seconds % 86400) / 3600),
        static_cast<int>((uptime_seconds % 3600) / 60),
        static_cast<int>(uptime_seconds % 60)
    };
}
inline void printVersionAndExit(const char* serviceName, int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << serviceName << "\n";
            std::cout << "Version: (not configured)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            std::exit(0);
        }
    }
}

// ── Metrics collector ─────────────────────────────────────────────────────

class Collector {
    static inline std::atomic<bool> s_requested{false};
    std::chrono::steady_clock::time_point start_;

    static void handleSIGUSR1(int) {
        s_requested.store(true, std::memory_order_release);
    }

public:
    void install() {
        start_ = std::chrono::steady_clock::now();
        std::signal(SIGUSR1, handleSIGUSR1);
    }

    bool poll() {
        return s_requested.exchange(false, std::memory_order_acq_rel);
    }

    void printHeader(const char* serviceName) {
        auto uptime = calculateUptime(start_);
        spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        spdlog::info("METRICS: {}", serviceName);
        spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}",
            uptime.days, uptime.hours, uptime.minutes, uptime.seconds);
    }

    void printFooter() {
        spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    }

    template<typename... Args>
    void printMetrics(const char* serviceName, Args&&... lines) {
        printHeader(serviceName);
        (spdlog::info("{}", std::forward<Args>(lines)), ...);
        printFooter();
    }
};

} // namespace metrics
} // namespace gtnh
