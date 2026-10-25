#include "itch/replay.hpp"
#include <algorithm>

namespace itch {

void ReplayBuilder::count(std::expected<Message, DecodeError> const& r) {
    ++stats_.messages;
    if (r) ++stats_.decoded;
    else if (r.error() == DecodeError::Ignored) ++stats_.ignored;
    else ++stats_.truncated;
}

void ReplayBuilder::feed(const Message& m) {
    if (auto* d = std::get_if<StockDirectory>(&m)) {
        if (to_string(d->stock) == symbol_) locate_ = d->locate;
        return;
    }
    // Every other message type we decode starts with the stock locate.
    const std::uint16_t loc = std::visit([](const auto& x) { return x.locate; }, m);
    if (loc != locate_) return;
    cur_ts_ = std::visit([](const auto& x) { return x.ts; }, m);

    if (auto* a = std::get_if<AddOrder>(&m)) {
        ++stats_.adds;
        add(a->ref, a->side == 'B' ? ob::Side::Buy : ob::Side::Sell, a->shares, a->price);
    } else if (auto* e = std::get_if<OrderExecuted>(&m)) {
        ++stats_.executions;
        shrink(e->ref, e->shares);
    } else if (auto* c = std::get_if<OrderCancel>(&m)) {
        ++stats_.cancels;
        shrink(c->ref, c->shares);
    } else if (auto* d = std::get_if<OrderDelete>(&m)) {
        ++stats_.deletes;
        remove(d->ref);
    } else if (auto* u = std::get_if<OrderReplace>(&m)) {
        ++stats_.replaces;
        auto it = live_.find(u->old_ref);
        if (it == live_.end()) { ++stats_.unknown_refs; return; }
        const ob::Side side = it->second.side;
        remove(u->old_ref);                       // replace = delete old + add new
        add(u->new_ref, side, u->shares, u->price);
    }
}

void ReplayBuilder::add(std::uint64_t ref, ob::Side side, std::uint32_t shares, std::uint32_t price) {
    const std::uint64_t id = next_id_++;
    live_[ref] = Live{id, side, shares, price};
    peak_live_ = std::max(peak_live_, live_.size());
    min_price_ = std::min<std::int64_t>(min_price_, price);
    max_price_ = std::max<std::int64_t>(max_price_, price);
    ops_.push_back({ReplayOp::Kind::Add,
                    ob::Order{ob::OrderId{id}, side, ob::OrderType::Limit,
                              ob::Price{price}, ob::Quantity{shares}}, cur_ts_});
}

void ReplayBuilder::shrink(std::uint64_t ref, std::uint32_t by) {
    auto it = live_.find(ref);
    if (it == live_.end()) { ++stats_.unknown_refs; return; }
    Live& l = it->second;
    if (by >= l.shares) { remove(ref); return; }
    l.shares -= by;
    ops_.push_back({ReplayOp::Kind::Reduce,
                    ob::Order{ob::OrderId{l.id}, l.side, ob::OrderType::Limit,
                              ob::Price{l.price}, ob::Quantity{l.shares}}, cur_ts_});
}

void ReplayBuilder::remove(std::uint64_t ref) {
    auto it = live_.find(ref);
    if (it == live_.end()) { ++stats_.unknown_refs; return; }
    ops_.push_back({ReplayOp::Kind::Cancel,
                    ob::Order{ob::OrderId{it->second.id}, it->second.side, ob::OrderType::Limit,
                              ob::Price{it->second.price}, ob::Quantity{0}}, cur_ts_});
    live_.erase(it);
}

}  // namespace itch
