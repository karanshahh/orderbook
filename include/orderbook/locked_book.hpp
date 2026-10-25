#pragma once
#include <mutex>
#include "orderbook/book.hpp"

namespace ob {

// Thread-safe wrapper: one mutex around the whole book.
// Exists to MEASURE the cost of locking, not to make the book faster.
// Matching is inherently sequential (every order depends on the book state
// left by the previous one), so a lock can only serialize threads, never
// run matching in parallel.
class LockedBook {
public:
    std::vector<Event> add(Order o) {
        std::scoped_lock lock(m_);
        return book_.add(o);
    }
    std::vector<Event> cancel(OrderId id) {
        std::scoped_lock lock(m_);
        return book_.cancel(id);
    }
    std::vector<Event> modify(OrderId id, Price p, Quantity q) {
        std::scoped_lock lock(m_);
        return book_.modify(id, p, q);
    }
    std::size_t open_orders() const {
        std::scoped_lock lock(m_);
        return book_.open_orders();
    }
    bool crossed() const {
        std::scoped_lock lock(m_);
        return book_.crossed();
    }

private:
    mutable std::mutex m_;
    Book book_;
};

}  // namespace ob
