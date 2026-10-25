#pragma once
// Turns one symbol's ITCH messages into order book operations.
//
// ITCH is the exchange's output: every add is a resting order, and executions,
// cancels and deletes shrink or remove it. Replaying that through our book
// rebuilds the exchange's book for one symbol. ITCH order reference numbers are
// large and global, so each order gets a small dense id (1, 2, 3, ...).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/itch.hpp"
#include "orderbook/order.hpp"

namespace itch {

struct ReplayOp {
    enum class Kind : std::uint8_t { Add, Cancel, Reduce } kind;
    ob::Order order;   // Add: the order. Cancel: id. Reduce: id, price, new remaining size.
};

struct ReplayStats {
    std::uint64_t messages = 0, decoded = 0, ignored = 0, truncated = 0;
    std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t unknown_refs = 0;   // touched an order we never saw added
};

class ReplayBuilder {
public:
    explicit ReplayBuilder(std::string symbol) : symbol_(std::move(symbol)) {}

    void feed(const Message& m);                 // call for every decoded message
    void count(std::expected<Message, DecodeError> const& r);

    bool found_symbol() const { return locate_ != kNoLocate; }
    const std::vector<ReplayOp>& ops() const { return ops_; }
    const ReplayStats& stats() const { return stats_; }

    std::int64_t  min_price() const { return min_price_; }
    std::int64_t  max_price() const { return max_price_; }
    std::uint64_t max_id() const { return next_id_; }     // all ids are below this
    std::size_t   peak_live() const { return peak_live_; }

private:
    static constexpr std::uint32_t kNoLocate = 0x1'0000;
    struct Live { std::uint64_t id; ob::Side side; std::uint32_t shares; std::uint32_t price; };

    void add(std::uint64_t ref, ob::Side side, std::uint32_t shares, std::uint32_t price);
    void shrink(std::uint64_t ref, std::uint32_t by);
    void remove(std::uint64_t ref);

    std::string symbol_;
    std::uint32_t locate_ = kNoLocate;
    std::unordered_map<std::uint64_t, Live> live_;
    std::vector<ReplayOp> ops_;
    ReplayStats stats_;
    std::uint64_t next_id_ = 1;
    std::int64_t  min_price_ = INT64_MAX, max_price_ = 0;
    std::size_t   peak_live_ = 0;
};

}  // namespace itch
