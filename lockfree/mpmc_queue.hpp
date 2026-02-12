#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <cstddef>
#include <type_traits>
#include <new>
#include <cassert>

namespace lockfree {

/**
 * @brief Multi-Producer Multi-Consumer bounded lock-free queue
 * 
 * Based on Dmitry Vyukov's MPMC algorithm.
 * Capacity MUST be a power of 2.
 * 
 * I will try to use it in a threadpool implementation
 */
template<typename T>
class MPMCQueue {
private:
    struct Node {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<size_t> sequence;
        
        T* data_ptr() noexcept { 
            return reinterpret_cast<T*>(storage); 
        }
        
        const T* data_ptr() const noexcept { 
            return reinterpret_cast<const T*>(storage); 
        }
    };

    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    // Ensure capacity is power of 2
    static size_t next_power_of_2(size_t n) {
        if (n == 0) return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }
    
    // Queue state
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_{0};
    alignas(CACHE_LINE_SIZE) const size_t capacity_;
    const size_t mask_;  // capacity_ - 1, for fast modulo
    Node* buffer_{nullptr};

public:
    /**
     * @brief Construct queue with given capacity
     * @param capacity Desired capacity (will be rounded up to next power of 2)
     */
    explicit MPMCQueue(size_t capacity) 
        : capacity_(next_power_of_2(capacity))
        , mask_(capacity_ - 1)
    {
        // Allocate memory for nodes
        buffer_ = static_cast<Node*>(::operator new(sizeof(Node) * capacity_, 
                                                    std::align_val_t{alignof(Node)}));
        
        // Initialize sequence numbers
        for (size_t i = 0; i < capacity_; ++i) {
            new (&buffer_[i]) Node();  // Placement new for Node
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPMCQueue() noexcept {
        if (!buffer_) return;
        
        // First, try to dequeue any remaining items
        while (true) {
            auto item = try_dequeue();
            if (!item) break;
            // Item is destroyed automatically by optional
        }
        
        // Destroy all nodes
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].~Node();
        }
        
        // Free memory
        ::operator delete(buffer_, std::align_val_t{alignof(Node)});
    }

    // Non-copyable, non-movable
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&) = delete;
    MPMCQueue& operator=(MPMCQueue&&) = delete;

    /**
     * @brief Attempt to enqueue an item (copy)
     * @return true if successful, false if queue is full
     */
    template<typename U>
    bool try_enqueue(U&& item) {
        return emplace_internal(std::forward<U>(item));
    }

    /**
     * @brief Attempt to construct an item in-place
     * @return true if successful, false if queue is full
     */
    template<typename... Args>
    bool try_emplace(Args&&... args) {
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Node* node;
        
        while (true) {
            node = &buffer_[pos & mask_];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                // Slot is available, try to claim it
                if (enqueue_pos_.compare_exchange_weak(
                    pos, pos + 1, 
                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                // Queue is full
                return false;
            } else {
                // Another thread claimed this slot, retry with fresh pos
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // Construct the item
        try {
            new (node->data_ptr()) T(std::forward<Args>(args)...);
        } catch (...) {
            // Construction failed, need to revert enqueue_pos?
            // This is complex - for now we'll leave slot unusable
            // Better approach: Use a sentinel value in sequence
            node->sequence.store(pos + capacity_, std::memory_order_release);
            throw;
        }
        
        // Mark item as ready for consumption
        node->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Attempt to dequeue an item
     * @return std::optional containing the item if successful, std::nullopt if empty
     */
    std::optional<T> try_dequeue() {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Node* node;
        
        while (true) {
            node = &buffer_[pos & mask_];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            
            if (diff == 0) {
                // Item is available, try to claim it
                if (dequeue_pos_.compare_exchange_weak(
                    pos, pos + 1,
                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                // Queue is empty
                return std::nullopt;
            } else {
                // Another thread already consumed or is consuming this item
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // Move item out
        T* item_ptr = node->data_ptr();
        std::optional<T> result;
        try {
            result.emplace(std::move(*item_ptr));
        } catch (...) {
            // Move failed, but we've already claimed the slot
            // Mark slot as unusable (poisoned)
            node->sequence.store(pos + capacity_, std::memory_order_release);
            throw;
        }
        
        // Destroy the moved-from object
        item_ptr->~T();
        
        // Mark slot as available for reuse
        // Adding capacity_ ensures sequence doesn't wrap to a lower value
        node->sequence.store(pos + capacity_, std::memory_order_release);
        
        return result;
    }
    
    /**
     * @brief Attempt to dequeue into existing object
     * @return true if successful, false if empty
     */
    bool try_dequeue(T& out) {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Node* node;
        
        while (true) {
            node = &buffer_[pos & mask_];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                    pos, pos + 1,
                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        
        // Move assign to output
        T* item_ptr = node->data_ptr();
        try {
            out = std::move(*item_ptr);
        } catch (...) {
            node->sequence.store(pos + capacity_, std::memory_order_release);
            throw;
        }
        
        item_ptr->~T();
        node->sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    /**
     * @brief Check if queue is empty (approximate)
     */
    bool empty() const noexcept {
        // This is only approximate - queue could become non-empty immediately after
        size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        return deq >= enq;
    }

    /**
     * @brief Get approximate size
     */
    size_t size() const noexcept {
        size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        
        // Both positions are monotonically increasing, so difference is size
        return enq - deq;
    }

    /**
     * @brief Check if queue is full (approximate)
     */
    bool full() const noexcept {
        return size() >= capacity_;
    }

    /**
     * @brief Get the capacity of the queue
     */
    size_t capacity() const noexcept { 
        return capacity_; 
    }

    /**
     * @brief Clear all items (not thread-safe - use only when no concurrent access)
     */
    void clear() noexcept {
        // Not truly thread-safe - for single-threaded cleanup only
        while (auto item = try_dequeue()) {
            // Items destroyed automatically
        }
    }

private:
    // Internal helper removed - emplace logic now in try_emplace
    template<typename U>
    bool emplace_internal(U&& item) {
        return try_emplace(std::forward<U>(item));
    }
};

} // namespace lockfree