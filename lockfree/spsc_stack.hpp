#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

template<typename T>
class LockFreeStack {
private:
    struct Node {
        T data;
        Node* next;
        Node(const T& value) : data(value), next(nullptr) {}
    };

    std::atomic<Node*> head;

public:
    LockFreeStack() : head(nullptr) {}

    void push(const T& value) {
        Node* new_node = new Node(value);
        new_node->next = head.load();

        while (!head.compare_exchange_weak(new_node->next, new_node));
    }

    bool try_pop(T& result) {
        Node* old_head = head.load();

        while (old_head != nullptr &&
               !head.compare_exchange_weak(old_head, old_head->next));

        if (old_head == nullptr) {
            return false;
        }

        std::cout << "Attempting to pop...\n";
        result = old_head->data;
        delete old_head;

        std::cout << "Popped: " << result << "\n";
        return true;
    }
};