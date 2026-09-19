#pragma once
#include <asio.hpp>
#include <mutex>
#include <pthread.h>
class NamedThreadPool {
public:
  static NamedThreadPool &instance() {
    static NamedThreadPool instance;
    return instance;
  }

  void addThread(asio::io_context &context, const std::string &name) {
    std::unique_lock lock(mutex_);
    threads_.emplace_back([&context, name] {
      pthread_setname_np(pthread_self(), name.c_str());
      context.run();
    });
  }

  template <typename F> void addThread(F &&func, const std::string &name) {
    std::unique_lock lock(mutex_);
    threads_.emplace_back([func = std::forward<F>(func), name] {
      pthread_setname_np(pthread_self(), name.c_str());
      func();
    });
  }

  ~NamedThreadPool() { join(); }

  void join() {
    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

private:
  NamedThreadPool() = default;
  std::vector<std::thread> threads_{3};
  std::mutex mutex_;
};
