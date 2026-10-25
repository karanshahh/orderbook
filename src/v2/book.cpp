#include "orderbook/v2/book.hpp"
#include <algorithm>

namespace ob::v2 {

Book::Book(const Config& cfg) : cfg_(cfg) {
    const std::size_t n_levels =
        static_cast<std::size_t>(cfg_.max_price.ticks - cfg_.min_price.ticks + 1);
    bids_.levels.resize(n_levels);
    asks_.levels.resize(n_levels);

    pool_.resize(cfg_.max_orders);
    for (std::uint32_t i = 0; i < cfg_.max_orders; ++i) pool_[i].next = i + 1;
    if (cfg_.max_orders > 0) {
        pool_.back().next = kNone;
        free_head_ = 0;
    }
    slot_of_.assign(cfg_.max_order_id, kNone);
}

// ---------------- public: orders ----------------

void Book::add(Order o, std::vector<Event>& out) {
    if (auto reason = validate(o)) {
        out.push_back(Rejected{o.id, *reason});
        return;
    }
    const Side opposite = o.side == Side::Buy ? Side::Sell : Side::Buy;
    if (o.type == OrderType::Market && side_of(opposite).best == kNoLevel) {
        out.push_back(Rejected{o.id, RejectReason::NoLiquidity});
        return;
    }

    out.push_back(Accepted{o.id});
    match(o, out);

    if (o.remaining.value > 0) {
        if (o.type == OrderType::Limit) rest(o);
        else out.push_back(Cancelled{o.id, o.remaining});
    }
}

void Book::cancel(OrderId id, std::vector<Event>& out) {
    if (id.value >= cfg_.max_order_id || slot_of_[id.value] == kNone) {
        out.push_back(Rejected{id, RejectReason::UnknownId});
        return;
    }
    const std::uint32_t slot = slot_of_[id.value];
    const Quantity remaining = pool_[slot].order.remaining;
    remove(slot);
    out.push_back(Cancelled{id, remaining});
}

void Book::modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Event>& out) {
    if (id.value >= cfg_.max_order_id || slot_of_[id.value] == kNone) {
        out.push_back(Rejected{id, RejectReason::UnknownId});
        return;
    }
    if (new_qty.value == 0) {
        out.push_back(Rejected{id, RejectReason::ZeroQuantity});
        return;
    }
    if (!in_range(new_price)) {            // check before touching the order
        out.push_back(Rejected{id, RejectReason::PriceOutOfRange});
        return;
    }

    const std::uint32_t slot = slot_of_[id.value];
    Node& node = pool_[slot];

    // Pure size reduction at the same price: keep time priority.
    if (new_price == node.order.price && new_qty <= node.order.remaining) {
        const std::uint64_t by = node.order.remaining.value - new_qty.value;
        node.order.remaining = new_qty;
        side_of(node.order.side).levels[index(new_price)].total -= by;
        out.push_back(Modified{id, new_price, new_qty});
        return;
    }

    // Anything else: cancel and re-add at the back of the queue.
    const Side side = node.order.side;
    remove(slot);
    add(Order{id, side, OrderType::Limit, new_price, new_qty}, out);
}

std::vector<Event> Book::add(Order o)                { std::vector<Event> v; add(o, v); return v; }
std::vector<Event> Book::cancel(OrderId id)          { std::vector<Event> v; cancel(id, v); return v; }
std::vector<Event> Book::modify(OrderId id, Price p, Quantity q) {
    std::vector<Event> v; modify(id, p, q, v); return v;
}

// ---------------- public: queries ----------------

std::optional<Price> Book::best_bid() const {
    if (bids_.best == kNoLevel) return std::nullopt;
    return Price{cfg_.min_price.ticks + bids_.best};
}

std::optional<Price> Book::best_ask() const {
    if (asks_.best == kNoLevel) return std::nullopt;
    return Price{cfg_.min_price.ticks + asks_.best};
}

Quantity Book::volume_at(Side side, Price price) const {
    if (!in_range(price)) return Quantity{};
    return Quantity{side_of(side).levels[index(price)].total};
}

bool Book::crossed() const {
    return bids_.best != kNoLevel && asks_.best != kNoLevel && bids_.best >= asks_.best;
}

std::size_t Book::open_orders() const { return open_; }

std::size_t Book::levels(Side side) const { return side_of(side).non_empty; }

std::optional<Order> Book::find(OrderId id) const {
    if (id.value >= cfg_.max_order_id || slot_of_[id.value] == kNone) return std::nullopt;
    return pool_[slot_of_[id.value]].order;
}

// ---------------- private ----------------

bool Book::in_range(Price p) const {
    return p.ticks >= cfg_.min_price.ticks && p.ticks <= cfg_.max_price.ticks;
}

std::size_t Book::index(Price p) const {
    return static_cast<std::size_t>(p.ticks - cfg_.min_price.ticks);
}

Book::Side_& Book::side_of(Side s) { return s == Side::Buy ? bids_ : asks_; }
const Book::Side_& Book::side_of(Side s) const { return s == Side::Buy ? bids_ : asks_; }

std::optional<RejectReason> Book::validate(const Order& o) const {
    if (o.remaining.value == 0)            return RejectReason::ZeroQuantity;
    if (o.id.value >= cfg_.max_order_id)   return RejectReason::IdOutOfRange;
    if (slot_of_[o.id.value] != kNone)     return RejectReason::DuplicateId;
    if (o.type == OrderType::Limit) {
        if (!in_range(o.price))            return RejectReason::PriceOutOfRange;
        if (free_head_ == kNone)           return RejectReason::BookFull;
    }
    return std::nullopt;
}

void Book::match(Order& incoming, std::vector<Event>& out) {
    const Side opp_side = incoming.side == Side::Buy ? Side::Sell : Side::Buy;
    Side_& opp = side_of(opp_side);

    while (incoming.remaining.value > 0 && opp.best != kNoLevel) {
        const std::size_t idx = static_cast<std::size_t>(opp.best);
        const Price level_price{cfg_.min_price.ticks + opp.best};
        if (incoming.type == OrderType::Limit) {
            const bool ok = incoming.side == Side::Buy ? level_price <= incoming.price
                                                       : level_price >= incoming.price;
            if (!ok) break;
        }

        Level& lvl = opp.levels[idx];
        while (incoming.remaining.value > 0 && lvl.head != kNone) {
            const std::uint32_t slot = lvl.head;          // earliest order first
            Order& resting = pool_[slot].order;
            const std::uint64_t qty = std::min(incoming.remaining.value, resting.remaining.value);

            out.push_back(Trade{resting.id, incoming.id, resting.price, Quantity{qty}});
            incoming.remaining.value -= qty;

            if (qty == resting.remaining.value) {
                unlink(lvl, slot);
                slot_of_[resting.id.value] = kNone;
                free_slot(slot);
                --open_;
            } else {
                resting.remaining.value -= qty;
                lvl.total -= qty;
            }
        }
        if (lvl.head == kNone) on_level_empty(opp_side, idx);
    }
}

void Book::rest(const Order& o) {
    const std::uint32_t slot = alloc_slot();
    pool_[slot].order = o;
    slot_of_[o.id.value] = slot;
    ++open_;

    Side_& s = side_of(o.side);
    const std::size_t idx = index(o.price);
    Level& lvl = s.levels[idx];
    if (lvl.head == kNone) {
        ++s.non_empty;
        const auto i = static_cast<std::int64_t>(idx);
        const bool better = s.best == kNoLevel ||
                            (o.side == Side::Buy ? i > s.best : i < s.best);
        if (better) s.best = i;
    }
    push_back(lvl, slot);
}

void Book::remove(std::uint32_t slot) {
    const Order o = pool_[slot].order;
    const std::size_t idx = index(o.price);
    Level& lvl = side_of(o.side).levels[idx];
    unlink(lvl, slot);
    slot_of_[o.id.value] = kNone;
    free_slot(slot);
    --open_;
    if (lvl.head == kNone) on_level_empty(o.side, idx);
}

// A level just emptied. If it was the best, walk outward to the next one.
void Book::on_level_empty(Side s, std::size_t idx) {
    Side_& side = side_of(s);
    --side.non_empty;
    if (static_cast<std::int64_t>(idx) != side.best) return;
    if (side.non_empty == 0) { side.best = kNoLevel; return; }

    std::int64_t i = side.best;
    if (s == Side::Buy) { do { --i; } while (side.levels[static_cast<std::size_t>(i)].head == kNone); }
    else                { do { ++i; } while (side.levels[static_cast<std::size_t>(i)].head == kNone); }
    side.best = i;
}

std::uint32_t Book::alloc_slot() {
    const std::uint32_t s = free_head_;
    free_head_ = pool_[s].next;
    return s;
}

void Book::free_slot(std::uint32_t slot) {
    pool_[slot].next = free_head_;
    free_head_ = slot;
}

void Book::push_back(Level& lvl, std::uint32_t slot) {
    Node& n = pool_[slot];
    n.prev = lvl.tail;
    n.next = kNone;
    if (lvl.tail != kNone) pool_[lvl.tail].next = slot;
    else                   lvl.head = slot;
    lvl.tail = slot;
    lvl.total += n.order.remaining.value;
}

void Book::unlink(Level& lvl, std::uint32_t slot) {
    Node& n = pool_[slot];
    if (n.prev != kNone) pool_[n.prev].next = n.next;
    else                 lvl.head = n.next;
    if (n.next != kNone) pool_[n.next].prev = n.prev;
    else                 lvl.tail = n.prev;
    lvl.total -= n.order.remaining.value;
}

}  // namespace ob::v2
