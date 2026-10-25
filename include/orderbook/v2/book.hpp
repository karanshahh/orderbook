#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "orderbook/events.hpp"
#include "orderbook/order.hpp"

namespace ob::v2 {

// Fixed limits, chosen up front. All memory is allocated in the constructor.
struct Config {
    Price         min_price{0};
    Price         max_price{100'000};         // inclusive
    std::uint32_t max_orders   = 1u << 20;    // resting orders at once (pool size)
    std::uint64_t max_order_id = 1u << 22;    // ids must be below this
};

// Same behavior and events as v1 (ob::Book), rebuilt for speed:
//   - order pool: a fixed array of nodes with a free list, no allocation per order
//   - intrusive FIFO: each node stores prev/next slot numbers, no std::list
//   - flat price index: one array slot per price tick, no std::map
//   - direct id lookup: an array indexed by order id, no hashing
//   - events are appended to a caller-owned vector, which keeps its capacity
//
// Extra rejects v1 never gives: PriceOutOfRange, IdOutOfRange, BookFull.
class Book {
public:
    explicit Book(const Config& cfg = {});

    // Hot-path API: events are appended to `out`.
    void add(Order o, std::vector<Event>& out);
    void cancel(OrderId id, std::vector<Event>& out);
    void modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Event>& out);

    // Convenience API, same shape as v1 (allocates a vector per call).
    std::vector<Event> add(Order o);
    std::vector<Event> cancel(OrderId id);
    std::vector<Event> modify(OrderId id, Price new_price, Quantity new_qty);

    // Queries
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity             volume_at(Side side, Price price) const;
    bool                 crossed() const;
    std::size_t          open_orders() const;
    std::size_t          levels(Side side) const;
    std::optional<Order> find(OrderId id) const;

private:
    static constexpr std::uint32_t kNone = 0xFFFF'FFFF;
    static constexpr std::int64_t  kNoLevel = -1;

    struct Node {                 // one resting order
        Order         order;
        std::uint32_t prev;       // slot of the order ahead in the queue
        std::uint32_t next;       // slot behind it (or next free slot)
    };
    struct Level {                // all orders at one price
        std::uint32_t head = kNone;
        std::uint32_t tail = kNone;
        std::uint64_t total = 0;
    };
    struct Side_ {                // one side of the book
        std::vector<Level> levels;
        std::int64_t       best = kNoLevel;   // index of best level
        std::size_t        non_empty = 0;
    };

    bool in_range(Price p) const;
    std::size_t index(Price p) const;
    Side_& side_of(Side s);
    const Side_& side_of(Side s) const;

    std::optional<RejectReason> validate(const Order& o) const;
    void match(Order& incoming, std::vector<Event>& out);
    void rest(const Order& o);
    void remove(std::uint32_t slot);              // unlink + free, no event
    void on_level_empty(Side s, std::size_t idx);

    std::uint32_t alloc_slot();
    void free_slot(std::uint32_t slot);
    void push_back(Level& lvl, std::uint32_t slot);
    void unlink(Level& lvl, std::uint32_t slot);

    Config                     cfg_;
    std::vector<Node>          pool_;
    std::uint32_t              free_head_ = kNone;
    std::vector<std::uint32_t> slot_of_;     // order id -> pool slot
    Side_                      bids_, asks_;
    std::size_t                open_ = 0;
};

}  // namespace ob::v2
