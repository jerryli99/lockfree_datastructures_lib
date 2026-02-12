#include <atomic>
#include <iostream>
#include <thread>
#include <memory>
#include <chrono>

template<typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(const T& value) : data(value), next(nullptr) {}
    };

    std::atomic<Node*> head;
    std::atomic<Node*> tail;

public:
    LockFreeQueue() {
        Node* dummy = new Node(T{});  // Dummy node to simplify push/pop logic
        head.store(dummy);
        tail.store(dummy);
    }

    ~LockFreeQueue() {
        while (Node* old_head = head.load()) {  // Cleanup all nodes
            head.store(old_head->next);
            delete old_head;
        }
    }

    void push(const T& value) {
        Node* new_node = new Node(value);
        Node* old_tail;

        while (true) {
            old_tail = tail.load();
            Node* tail_next = old_tail->next;

            if (old_tail == tail.load()) {  // Check if tail hasn't changed
                if (tail_next == nullptr) {  // Tail is truly the last node
                    if (old_tail->next.compare_exchange_weak(tail_next, new_node)) {
                        break;  // Successfully added the new node
                    }
                } else {  // Tail is lagging; advance it
                    tail.compare_exchange_weak(old_tail, tail_next);
                }
            }
        }
        // Move tail to the new node
        tail.compare_exchange_weak(old_tail, new_node);
        std::cout << "Pushed: " << value << "\n";
    }

    bool pop(T& result) {
        Node* old_head;

        while (true) {
            old_head = head.load();
            Node* old_tail = tail.load();
            Node* head_next = old_head->next;

            if (old_head == head.load()) {  // Consistency check
                if (old_head == old_tail) {  // Queue might be empty
                    if (head_next == nullptr) {  // Queue is empty
                        std::cout << "Pop failed - queue is empty.\n";
                        return false;
                    }
                    tail.compare_exchange_weak(old_tail, head_next);  // Advance tail
                } else {  // Queue is not empty
                    if (head.compare_exchange_weak(old_head, head_next)) {
                        result = head_next->data;
                        delete old_head;  // Free the old dummy head node
                        std::cout << "Popped: " << result << "\n";
                        return true;
                    }
                }
            }
        }
    }
};