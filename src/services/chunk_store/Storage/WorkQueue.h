#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

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