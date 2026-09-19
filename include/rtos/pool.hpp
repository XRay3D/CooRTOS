// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstddef>
#include <new>

#include "config.hpp"
#include "port.hpp"

// Аллокатор фреймов корутин: статический пул фиксированных блоков (по
// умолчанию) либо обычная куча (-DRTOS_USE_MALLOC).

namespace rtos {

class FramePool {
public:
    void* allocate(std::size_t n) {
        if (n > BlockSize) {
            return nullptr;
        }
        port::CriticalSection cs;
        if (!initialized_) {
            for (std::size_t i = BlockCount; i > 0; --i) {
                blocks_[i - 1].next = free_;
                free_ = &blocks_[i - 1];
            }
            initialized_ = true;
        }
        Block* b = free_;
        if (b) {
            free_ = b->next;
        }
        return b;
    }

    void deallocate(void* p) {
        if (!p) {
            return;
        }
        // Указатель обязан быть началом блока этого пула.
        // (в предикате контракта this и параметры — const)
        RTOS_CONTRACT_ASSERT(p >= static_cast<const void*>(blocks_) &&
                             p < static_cast<const void*>(blocks_ + BlockCount));
        RTOS_CONTRACT_ASSERT(
            (static_cast<std::size_t>(
                 static_cast<const std::byte*>(p) -
                 reinterpret_cast<const std::byte*>(blocks_)) %
             sizeof(Block)) == 0);
        port::CriticalSection cs;
        Block* b = static_cast<Block*>(p);
        b->next = free_;
        free_ = b;
    }

    std::size_t free_count() const {
        std::size_t n = 0;
        for (Block* b = free_; b; b = b->next) {
            ++n;
        }
        return n;
    }

private:
    static constexpr std::size_t BlockSize = RTOS_FRAME_BLOCK_SIZE;
    static constexpr std::size_t BlockCount = RTOS_FRAME_BLOCK_COUNT;

    union Block {
        Block* next;
        alignas(std::max_align_t) std::byte data[BlockSize];
    };

    Block blocks_[BlockCount];
    Block* free_ = nullptr;
    bool initialized_ = false;
};

inline FramePool frame_pool;

namespace config {

inline void* allocate(std::size_t n) noexcept {
#ifdef RTOS_USE_MALLOC
    void* p = ::operator new(n, std::nothrow);
#else
    void* p = frame_pool.allocate(n);
#endif
    if (!p) {
        if (alloc_failed_hook) {
            alloc_failed_hook(n);
        } else {
            RTOS_TRAP();
        }
    }
    return p;
}

inline void deallocate(void* p, std::size_t) noexcept {
#ifdef RTOS_USE_MALLOC
    ::operator delete(p);
#else
    frame_pool.deallocate(p);
#endif
}

} // namespace config

} // namespace rtos
