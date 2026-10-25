#include "orderbook/book.hpp"
#include <algorithm>

namespace ob {

std::vector<Event> Book::add(Order o) {
    if (auto reason = validate(o)) {
        return {Rejected{o.id, *reason}};
    }

    const bool buy = (o.side == Side::Buy);
    if (o.type == OrderType::Market && (buy ? asks_.empty() : bids_.empty())) {
        return {Rejected{o.id, RejectReason::NoLiquidity}};
    }

    std::vector<Event> out;
    out.push_back(Accepted{o.id});

    if (buy) match(o, asks_, out);
    else     match(o, bids_, out);

    if (o.remaining.value > 0) {
        if (o.type == OrderType::Limit) rest(o);
        else out.push_back(Cancelled{o.id, o.remaining});
    }
    return out;
}

std::vector<Event> Book::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return {Rejected{id, RejectReason::UnknownId}};
    }
    const Locator loc = it->second;
    const Quantity remaining = loc.handle->remaining;   // read before erase

    if (loc.side == Side::Buy) remove(bids_, loc);
    else                       remove(asks_, loc);
    index_.erase(it);

    return {Cancelled{id, remaining}};
}

std::vector<Event> Book::modify(OrderId id, Price new_price, Quantity new_qty) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return {Rejected{id, RejectReason::UnknownId}};
    }
    if (new_qty.value == 0) {
        return {Rejected{id, RejectReason::ZeroQuantity}};
    }

    const Locator loc = it->second;
    const Quantity current = loc.handle->remaining;

    // Pure size reduction at the same price: keep time priority.
    if (new_price == loc.price && new_qty <= current) {
        const Quantity by{current.value - new_qty.value};
        if (by.value > 0) {
            if (loc.side == Side::Buy) bids_.find(loc.price)->second.reduce(loc.handle, by);
            else                       asks_.find(loc.price)->second.reduce(loc.handle, by);
        }
        return {Modified{id, new_price, new_qty}};
    }

    // Anything else: cancel and re-add at the back of the queue.
    const Side side = loc.side;
    if (side == Side::Buy) remove(bids_, loc);
    else                   remove(asks_, loc);
    index_.erase(it);

    return add(Order{id, side, OrderType::Limit, new_price, new_qty});
}

std::optional<Price> Book::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> Book::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity Book::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? Quantity{} : it->second.total();
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? Quantity{} : it->second.total();
}

bool Book::crossed() const {
    auto bb = best_bid();
    auto ba = best_ask();
    return bb && ba && *bb >= *ba;
}

std::size_t Book::open_orders() const { return index_.size(); }

std::size_t Book::levels(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::optional<RejectReason> Book::validate(const Order& o) const {
    if (o.remaining.value == 0) return RejectReason::ZeroQuantity;
    if (index_.contains(o.id))  return RejectReason::DuplicateId;
    return std::nullopt;
}

bool Book::can_match(const Order& incoming, Price resting_price) {
    if (incoming.type == OrderType::Market) return true;
    return incoming.side == Side::Buy ? resting_price <= incoming.price
                                      : resting_price >= incoming.price;
}

template <typename Levels>
void Book::match(Order& incoming, Levels& opposite, std::vector<Event>& out) {
    while (incoming.remaining.value > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();                 // best price first
        if (!can_match(incoming, level_it->first)) break;

        PriceLevel& level = level_it->second;
        while (incoming.remaining.value > 0 && !level.empty()) {
            Order& resting = level.front();               // earliest first
            const Quantity qty{std::min(incoming.remaining.value, resting.remaining.value)};

            out.push_back(Trade{resting.id, incoming.id, resting.price, qty});  // resting price
            incoming.remaining.value -= qty.value;

            if (qty == resting.remaining) {
                index_.erase(resting.id);                 // before the order is destroyed
                level.erase(level.front_handle());
            } else {
                level.reduce(level.front_handle(), qty);
            }
        }
        if (level.empty()) opposite.erase(level_it);      // no empty levels
    }
}

template <typename Levels>
void Book::remove(Levels& levels, const Locator& loc) {
    auto level_it = levels.find(loc.price);
    level_it->second.erase(loc.handle);
    if (level_it->second.empty()) levels.erase(level_it);
}

void Book::rest(const Order& o) {
    PriceLevel::Handle h = (o.side == Side::Buy) ? bids_[o.price].append(o)
                                                 : asks_[o.price].append(o);
    index_.emplace(o.id, Locator{o.side, o.price, h});
}

}  // namespace ob
