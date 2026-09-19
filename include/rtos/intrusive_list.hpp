// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

namespace rtos {

// Интрузивный узел двусвязного кольцевого списка.
// Инвариант: узел, зацикленный сам на себя, не состоит ни в одном списке,
// поэтому unlink() безусловен и идемпотентен.
struct ListNode {
    ListNode* prev = this;
    ListNode* next = this;
    void*     owner = nullptr; // объект-владелец (TaskPromise / WaitNode)

    ListNode() = default;
    explicit ListNode(void* own) : owner(own) {}
    ListNode(const ListNode&) = delete;
    ListNode& operator=(const ListNode&) = delete;

    bool linked() const { return next != this; }

    void unlink() {
        prev->next = next;
        next->prev = prev;
        prev = next = this;
    }

    void insert_before(ListNode& pos) {
        prev = pos.prev;
        next = &pos;
        pos.prev->next = this;
        pos.prev = this;
    }
};

// Кольцевой список с фиктивной головой.
struct List {
    ListNode head;

    bool empty() const { return !head.linked(); }

    void push_back(ListNode& n) { n.insert_before(head); }

    ListNode* front() { return empty() ? nullptr : head.next; }

    ListNode* pop_front() {
        if (empty()) {
            return nullptr;
        }
        ListNode* n = head.next;
        n->unlink();
        return n;
    }
};

} // namespace rtos
