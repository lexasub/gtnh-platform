#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <memory>

class ConcurrentMemoryPool {
public:
    static constexpr size_t kDefaultSize = 256 * 1024;

    struct Handle {
        uint8_t* data = nullptr;
        size_t size = 0;
        size_t capacity = 0;

        bool valid() const { return data != nullptr; }
    };

    ConcurrentMemoryPool();
    ~ConcurrentMemoryPool();

    ConcurrentMemoryPool(const ConcurrentMemoryPool&) = delete;
    ConcurrentMemoryPool& operator=(const ConcurrentMemoryPool&) = delete;

    Handle acquire(size_t min_size);
    void release(Handle& h);

private:
    struct Block {
        std::atomic<bool> in_use{false};
        uint8_t* data = nullptr;
        size_t capacity = 0;

        Block() = default;

        // Конструктор для emplace_back
        Block(bool used, uint8_t* d, size_t cap)
            : data(d), capacity(cap) {
            in_use.store(used, std::memory_order_relaxed);
        }

        // Не копируемый, не movable из-за atomic
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        Block(Block&&) = delete;
        Block& operator=(Block&&) = delete;
    };

    std::vector<std::unique_ptr<Block>> blocks_;
    std::mutex grow_mutex_;
    size_t num_initialized_ = 0;

    Block* try_acquire_existing(size_t min_size);

    // Бинарный поиск блока по адресу данных
    Block* find_block_by_data(uint8_t* ptr);
};
