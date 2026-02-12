#pragma once

#include <memory>
#include <mutex>
#include <condition_variable>
#include <concepts>
#include <ranges>
#include <utility>
#include <type_traits>
#include <stdexcept>

/**
 * @brief Thread-safe lock-based concurrent Queue
 * 
 */
namespace Concurrent {

template<typename T, class Allocator = std::allocator<T>>
class Queue {
private:
    struct Node {
        T data;
        Node* next;
        
        template<typename... Args>
        Node(Args&&... args) 
            : data(std::forward<Args>(args)...), next(nullptr) {}
    };
    
    using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    
    Node* head;
    Node* tail;
    std::size_t count;
    
    mutable std::mutex mtx;
    std::condition_variable cv;
    
    NodeAllocator alloc;
    
    Node* allocate_node() {
        return std::allocator_traits<NodeAllocator>::allocate(alloc, 1);
    }
    
    void deallocate_node(Node* node) {
        std::allocator_traits<NodeAllocator>::deallocate(alloc, node, 1);
    }
    
    template<typename... Args>
    Node* create_node(Args&&... args) {
        Node* node = allocate_node();
        try {
            std::allocator_traits<NodeAllocator>::construct(alloc, node, std::forward<Args>(args)...);
        } catch (...) {
            deallocate_node(node);
            throw;
        }
        return node;
    }
    
    void destroy_node(Node* node) noexcept {
        std::allocator_traits<NodeAllocator>::destroy(alloc, node);
        deallocate_node(node);
    }
    
    void clear() noexcept {
        while (head) {
            Node* temp = head;
            head = head->next;
            destroy_node(temp);
        }
        tail = nullptr;
        count = 0;
    }

public:
    // Constructors
    Queue() noexcept(noexcept(NodeAllocator())) 
        : head(nullptr), tail(nullptr), count(0), alloc() {}
    
    explicit Queue(const Allocator& allocator) noexcept 
        : head(nullptr), tail(nullptr), count(0), alloc(allocator) {}
    
    Queue(const Queue& other) 
        : head(nullptr), tail(nullptr), count(0), alloc(
            std::allocator_traits<NodeAllocator>::select_on_container_copy_construction(other.alloc)
        ) {
        std::lock_guard lock(other.mtx);
        Node* current = other.head;
        while (current) {
            push(current->data);
            current = current->next;
        }
    }
    
    Queue(Queue&& other) noexcept 
        : head(nullptr), tail(nullptr), count(0), alloc(std::move(other.alloc)) {
        std::lock_guard lock(other.mtx);
        head = other.head;
        tail = other.tail;
        count = other.count;
        
        other.head = nullptr;
        other.tail = nullptr;
        other.count = 0;
    }
    
    ~Queue() {
        clear();
    }
    
    // Assignment operators
    Queue& operator=(const Queue& other) {
        if (this != &other) {
            std::scoped_lock lock(mtx, other.mtx);
            clear();
            
            if constexpr (std::allocator_traits<NodeAllocator>::propagate_on_container_copy_assignment::value) {
                alloc = other.alloc;
            }
            
            Node* current = other.head;
            while (current) {
                Node* new_node = create_node(current->data);
                if (!head) {
                    head = tail = new_node;
                } else {
                    tail->next = new_node;
                    tail = new_node;
                }
                ++count;
                current = current->next;
            }
        }
        return *this;
    }
    
    Queue& operator=(Queue&& other) noexcept(
        std::allocator_traits<NodeAllocator>::propagate_on_container_move_assignment::value ||
        std::allocator_traits<NodeAllocator>::is_always_equal::value
    ) {
        if (this != &other) {
            std::scoped_lock lock(mtx, other.mtx);
            clear();
            
            if constexpr (std::allocator_traits<NodeAllocator>::propagate_on_container_move_assignment::value) {
                alloc = std::move(other.alloc);
                head = other.head;
                tail = other.tail;
                count = other.count;
            } else if (alloc == other.alloc) {
                head = other.head;
                tail = other.tail;
                count = other.count;
            } else {
                Node* current = other.head;
                while (current) {
                    Node* new_node = create_node(std::move(current->data));
                    if (!head) {
                        head = tail = new_node;
                    } else {
                        tail->next = new_node;
                        tail = new_node;
                    }
                    ++count;
                    current = current->next;
                }
                other.clear();
                return *this;
            }
            
            other.head = nullptr;
            other.tail = nullptr;
            other.count = 0;
        }
        return *this;
    }
    
    // Element access
    T& front() {
        std::lock_guard lock(mtx);
        if (!head) {
            throw std::runtime_error("Queue is empty");
        }
        return head->data;
    }
    
    const T& front() const {
        std::lock_guard lock(mtx);
        if (!head) {
            throw std::runtime_error("Queue is empty");
        }
        return head->data;
    }
    
    T& back() {
        std::lock_guard lock(mtx);
        if (!tail) {
            throw std::runtime_error("Queue is empty");
        }
        return tail->data;
    }
    
    const T& back() const {
        std::lock_guard lock(mtx);
        if (!tail) {
            throw std::runtime_error("Queue is empty");
        }
        return tail->data;
    }
    
    // Capacity
    bool empty() const noexcept {
        std::lock_guard lock(mtx);
        return count == 0;
    }
    
    std::size_t size() const noexcept {
        std::lock_guard lock(mtx);
        return count;
    }
    
    // Modifiers
    template<typename U>
    void push(U&& value) {
        Node* new_node = create_node(std::forward<U>(value));
        {
            std::lock_guard lock(mtx);
            if (!head) {
                head = tail = new_node;
            } else {
                tail->next = new_node;
                tail = new_node;
            }
            ++count;
        }
        cv.notify_one();
    }
    
    template<std::ranges::input_range Range>
    void push_range(Range&& range) {
        auto it = std::ranges::begin(range);
        auto end = std::ranges::end(range);
        
        if (it == end) return;
        
        // Create first node
        Node* first = create_node(*it);
        Node* last = first;
        ++it;
        
        // Create remaining nodes
        while (it != end) {
            Node* new_node = create_node(*it);
            last->next = new_node;
            last = new_node;
            ++it;
        }
        
        // Link into queue
        {
            std::lock_guard lock(mtx);
            if (!head) {
                head = first;
            } else {
                tail->next = first;
            }
            tail = last;
            count += std::ranges::distance(range);
        }
        cv.notify_all();
    }
    
    template<typename... Args>
    decltype(auto) emplace(Args&&... args) {
        Node* new_node = create_node(std::forward<Args>(args)...);
        {
            std::lock_guard lock(mtx);
            if (!head) {
                head = tail = new_node;
            } else {
                tail->next = new_node;
                tail = new_node;
            }
            ++count;
        }
        cv.notify_one();
        return tail->data;
    }
    
    void pop() {
        std::unique_lock lock(mtx);
        cv.wait(lock, [this] { return head != nullptr; });
        
        Node* temp = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }
        --count;
        
        lock.unlock();
        destroy_node(temp);
    }
    
    bool try_pop(T& value) {
        std::lock_guard lock(mtx);
        if (!head) {
            return false;
        }
        
        value = std::move(head->data);
        Node* temp = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }
        --count;
        
        destroy_node(temp);
        return true;
    }
    
    template<typename Rep, typename Period>
    bool pop_for(T& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock lock(mtx);
        if (!cv.wait_for(lock, timeout, [this] { return head != nullptr; })) {
            return false;
        }
        
        value = std::move(head->data);
        Node* temp = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }
        --count;
        
        lock.unlock();
        destroy_node(temp);
        return true;
    }
    
    template<typename Clock, typename Duration>
    bool pop_until(T& value, const std::chrono::time_point<Clock, Duration>& timeout_time) {
        std::unique_lock lock(mtx);
        if (!cv.wait_until(lock, timeout_time, [this] { return head != nullptr; })) {
            return false;
        }
        
        value = std::move(head->data);
        Node* temp = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }
        --count;
        
        lock.unlock();
        destroy_node(temp);
        return true;
    }
    
    void swap(Queue& other) noexcept(
        std::allocator_traits<NodeAllocator>::propagate_on_container_swap::value ||
        std::allocator_traits<NodeAllocator>::is_always_equal::value
    ) {
        if (this != &other) {
            std::scoped_lock lock(mtx, other.mtx);
            
            if constexpr (std::allocator_traits<NodeAllocator>::propagate_on_container_swap::value) {
                using std::swap;
                swap(alloc, other.alloc);
            }
            
            std::swap(head, other.head);
            std::swap(tail, other.tail);
            std::swap(count, other.count);
        }
    }
    
    // Additional utility methods
    void wait_and_pop(T& value) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [this] { return head != nullptr; });
        
        value = std::move(head->data);
        Node* temp = head;
        head = head->next;
        if (!head) {
            tail = nullptr;
        }
        --count;
        
        lock.unlock();
        destroy_node(temp);
    }
    
    // Thread-safe clear
    void clear_queue() {
        std::lock_guard lock(mtx);
        clear();
    }
    
    // Bulk operations for better performance
    template<typename OutputIt>
    std::size_t pop_bulk(OutputIt dest, std::size_t max_count) {
        std::lock_guard lock(mtx);
        std::size_t popped = 0;
        
        while (head && popped < max_count) {
            *dest++ = std::move(head->data);
            Node* temp = head;
            head = head->next;
            destroy_node(temp);
            ++popped;
            --count;
        }
        
        if (!head) {
            tail = nullptr;
        }
        
        if (popped > 0) {
            cv.notify_all();
        }
        
        return popped;
    }
    
    Allocator get_allocator() const noexcept {
        return alloc;
    }
    
    // For compatibility with standard containers
    friend void swap(Queue& a, Queue& b) noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }
};

} // namespace Concurrent