#pragma once

#include <atomic>
#include <optional>
#include <cstddef>
#include <type_traits>
#include <new>
#include <cassert>

namespace Lockfree {

/**
 * @brief Lock-free ring buffer (SPSC)
 * 
 * Fixed-size circular buffer optimized for single producer, single consumer.
 * Uses power-of-2 capacity for optimal performance.
 */
template<typename T>
class RingBuffer {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    // Ensure capacity is power of 2 for fast modulo
    static constexpr size_t next_power_of_2(size_t n) noexcept {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }
    
    // Storage for elements
    alignas(alignof(T)) 
    std::byte* storage_;  // Raw storage for T objects
    
    size_t capacity_;
    size_t mask_;  // capacity_ - 1 for fast modulo
    
    // Producer cache line
    alignas(CACHE_LINE_SIZE) 
    std::atomic<size_t> write_pos_{0};
    size_t cached_read_pos_{0};  // Producer reads consumer's position
    
    char padding1_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];
    
    // Consumer cache line
    alignas(CACHE_LINE_SIZE) 
    std::atomic<size_t> read_pos_{0};
    size_t cached_write_pos_{0};  // Consumer reads producer's position
    
    char padding2_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];

public:
    /**
     * @brief Construct ring buffer with given capacity
     * @param capacity Desired capacity (will be rounded up to next power of 2)
     */
    explicit RingBuffer(size_t capacity)
        : capacity_(next_power_of_2(capacity))
        , mask_(capacity_ - 1)
        , write_pos_(0)
        , cached_read_pos_(0)
        , read_pos_(0)
        , cached_write_pos_(0)
    {
        // Allocate raw storage for T objects
        storage_ = static_cast<std::byte*>(
            ::operator new(sizeof(T) * capacity_, std::align_val_t{alignof(T)})
        );
    }

    ~RingBuffer() noexcept {
        // Destroy any remaining elements
        if constexpr (!std::is_trivially_destructible_v<T>) {
            size_t read = read_pos_.load(std::memory_order_relaxed);
            size_t write = write_pos_.load(std::memory_order_relaxed);
            
            while (read != write) {
                T* ptr = reinterpret_cast<T*>(storage_ + ((read & mask_) * sizeof(T)));
                ptr->~T();
                ++read;
            }
        }
        
        ::operator delete(storage_, std::align_val_t{alignof(T)});
    }

    // Non-copyable, non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /**
     * @brief Try to write an item (producer only)
     * @return true if successful, false if buffer is full
     */
    template<typename U>
    bool try_write(U&& item) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        static_assert(std::is_constructible_v<T, U&&>,
                     "Cannot construct T from provided argument");
        
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t read = cached_read_pos_;
        
        // Check if full
        if ((write - read) >= capacity_) {
            // Refresh cache with acquire semantics
            read = read_pos_.load(std::memory_order_acquire);
            cached_read_pos_ = read;
            if ((write - read) >= capacity_) {
                return false;  // Buffer is full
            }
        }
        
        // Construct element in-place
        T* ptr = reinterpret_cast<T*>(storage_ + ((write & mask_) * sizeof(T)));
        try {
            new (ptr) T(std::forward<U>(item));
        } catch (...) {
            // Construction failed, don't increment write position
            return false;
        }
        
        // Publish write with release semantics
        write_pos_.store(write + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Try to construct an item in-place (producer only)
     * @return true if successful, false if buffer is full
     */
    template<typename... Args>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        static_assert(std::is_constructible_v<T, Args...>,
                     "Cannot construct T from provided arguments");
        
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t read = cached_read_pos_;
        
        if ((write - read) >= capacity_) {
            read = read_pos_.load(std::memory_order_acquire);
            cached_read_pos_ = read;
            if ((write - read) >= capacity_) {
                return false;
            }
        }
        
        T* ptr = reinterpret_cast<T*>(storage_ + ((write & mask_) * sizeof(T)));
        try {
            new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            return false;
        }
        
        write_pos_.store(write + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Try to read an item (consumer only)
     * @return std::optional containing the item if successful, std::nullopt if empty
     */
    std::optional<T> try_read() noexcept(std::is_nothrow_move_constructible_v<T>) {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        size_t write = cached_write_pos_;
        
        // Check if empty
        if (read >= write) {
            // Refresh cache with acquire semantics
            write = write_pos_.load(std::memory_order_acquire);
            cached_write_pos_ = write;
            if (read >= write) {
                return std::nullopt;  // Buffer is empty
            }
        }
        
        // Read element
        T* ptr = reinterpret_cast<T*>(storage_ + ((read & mask_) * sizeof(T)));
        
        // Move construct the result
        std::optional<T> result;
        try {
            result.emplace(std::move(*ptr));
        } catch (...) {
            // Move construction failed, leave buffer unchanged
            return std::nullopt;
        }
        
        // Destroy the moved-from object
        ptr->~T();
        
        // Publish read with release semantics
        read_pos_.store(read + 1, std::memory_order_release);
        
        return result;
    }

    /**
     * @brief Try to read an item into provided reference (consumer only)
     * @return true if successful, false if empty
     */
    bool try_read(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        size_t write = cached_write_pos_;
        
        if (read >= write) {
            write = write_pos_.load(std::memory_order_acquire);
            cached_write_pos_ = write;
            if (read >= write) {
                return false;
            }
        }
        
        T* ptr = reinterpret_cast<T*>(storage_ + ((read & mask_) * sizeof(T)));
        try {
            out = std::move(*ptr);
        } catch (...) {
            return false;
        }
        
        ptr->~T();
        read_pos_.store(read + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Peek at front element without removing (consumer only)
     */
    const T* peek() const noexcept {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        size_t write = write_pos_.load(std::memory_order_acquire);
        
        if (read >= write) {
            return nullptr;
        }
        
        return reinterpret_cast<const T*>(storage_ + ((read & mask_) * sizeof(T)));
    }

    /**
     * @brief Check if buffer is empty (consumer only)
     */
    bool empty() const noexcept {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        size_t write = write_pos_.load(std::memory_order_acquire);
        return read >= write;
    }

    /**
     * @brief Check if buffer is full (producer only)
     */
    bool full() const noexcept {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t read = read_pos_.load(std::memory_order_acquire);
        return (write - read) >= capacity_;
    }

    /**
     * @brief Get approximate size
     * Note: This is approximate because producer/consumer may be concurrently modifying
     */
    size_t size() const noexcept {
        size_t write = write_pos_.load(std::memory_order_acquire);
        size_t read = read_pos_.load(std::memory_order_acquire);
        return write - read;
    }

    /**
     * @brief Get available space
     * Note: This is approximate because producer/consumer may be concurrently modifying
     */
    size_t available() const noexcept {
        return capacity_ - size();
    }

    /**
     * @brief Get the capacity
     */
    size_t capacity() const noexcept { 
        return capacity_; 
    }

    /**
     * @brief Clear the buffer (not thread-safe - only when no concurrent access)
     */
    void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            size_t read = read_pos_.load(std::memory_order_relaxed);
            size_t write = write_pos_.load(std::memory_order_relaxed);
            
            while (read != write) {
                T* ptr = reinterpret_cast<T*>(storage_ + ((read & mask_) * sizeof(T)));
                ptr->~T();
                ++read;
            }
        }
        
        read_pos_.store(0, std::memory_order_relaxed);
        write_pos_.store(0, std::memory_order_relaxed);
        cached_read_pos_ = 0;
        cached_write_pos_ = 0;
    }
};

} // namespace Lockfree