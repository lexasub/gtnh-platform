#include "WorkQueue.h"

WorkQueue::WorkQueue(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&WorkQueue::workerLoop, this);
    }
}

void WorkQueue::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void WorkQueue::stop() {
    stop_ = true;
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void WorkQueue::workerLoop() {
    while (!stop_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !tasks_.empty() || stop_; });
            if (stop_ && tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}
