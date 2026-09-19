#include "ConcurrentMemoryPool.h"
#include <memory>

ConcurrentMemoryPool::ConcurrentMemoryPool() {
    blocks_.reserve(4);
}

ConcurrentMemoryPool::~ConcurrentMemoryPool() {
    for (auto& b : blocks_) {
        delete[] b->data;
    }
}

ConcurrentMemoryPool::Block* ConcurrentMemoryPool::try_acquire_existing(size_t min_size) {
    const size_t n = num_initialized_;
    for (size_t i = 0; i < n; ++i) {
        Block& b = *blocks_[i];
        if (b.capacity < min_size) continue;
        
        bool expected = false;
        if (b.in_use.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return &b;
        }
    }
    return nullptr;
}

ConcurrentMemoryPool::Block* ConcurrentMemoryPool::find_block_by_data(uint8_t* ptr) {
    if (!ptr || blocks_.empty()) return nullptr;
    
    // Бинарный поиск: blocks_ отсортированы по data (адреса растут)
    size_t lo = 0, hi = blocks_.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        auto* mid_block = blocks_[mid].get();
        if (mid_block->data < ptr) {
            lo = mid + 1;
        } else  {
            hi = mid;
        }
    }
    // lo — первый блок с data >= ptr
    if (lo < blocks_.size()) {
        auto* block = blocks_[lo].get();
        if (block->data == ptr) {
            return block; // точное совпадение
        }
    }

    // Проверяем предыдущий блок: ptr может лежать внутри [data, data+capacity)
    if (lo <= 0) {
        return nullptr;
    }
    auto* block = blocks_[lo - 1].get();
    // branchless range check через беззнаковую арифметику
    // (ptr >= block->data гарантировано свойством lower_bound)
    uintptr_t offset = reinterpret_cast<uintptr_t>(ptr)
                       - reinterpret_cast<uintptr_t>(block->data);
    return (offset < block->capacity) ? block : nullptr;
}

ConcurrentMemoryPool::Handle ConcurrentMemoryPool::acquire(size_t min_size) {
    if (min_size == 0) return {};

    Block* b = try_acquire_existing(min_size);
    if (b) return {b->data, 0, b->capacity};

    std::lock_guard<std::mutex> lock(grow_mutex_);
    
    b = try_acquire_existing(min_size);
    if (b) return {b->data, 0, b->capacity};

    size_t new_cap = (min_size <= kDefaultSize) ? kDefaultSize : min_size;

    uint8_t* ptr = new uint8_t[new_cap];

    blocks_.emplace_back(std::make_unique<Block>(true, ptr, new_cap));
    num_initialized_ = blocks_.size();
    
    // Поддерживаем инвариант сортировки (обычно уже отсортировано, но на всякий)
    // На самом деле new обычно даёт возрастающие адреса, но не гарантирует
    // Вставляем в правильное место или просто push_back если адрес больше
    if (blocks_.size() > 1 && blocks_[blocks_.size() - 2]->data > ptr) {
        // Редкий случай — сортируем
        std::ranges::sort(blocks_,
                          [](const std::unique_ptr<Block>& a, const std::unique_ptr<Block>& b) { return a->data < b->data; });
    }
    
    return {ptr, 0, new_cap};
}

void ConcurrentMemoryPool::release(Handle& h) {
    if (!h.valid()) return;

    Block* b = find_block_by_data(h.data);
    if (b) {
        b->in_use.store(false, std::memory_order_release);
    }
    h = {};
}