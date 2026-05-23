#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <vector>
#include <atomic>

class WorkQueue {
public:
    explicit WorkQueue(size_t num_threads = 2);

    ~WorkQueue() { stop(); }

    void enqueue(std::function<void()> task);

    void stop();

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};