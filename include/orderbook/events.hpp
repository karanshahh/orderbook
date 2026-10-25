#pragma once
#include <variant>
#include "orderbook/types.hpp"

namespace ob {

enum class RejectReason : std::uint8_t {
    DuplicateId,      // id already resting in the book
    ZeroQuantity,     // quantity must be > 0
    UnknownId,        // cancel/modify of an id that is not resting
    NoLiquidity,      // market order with nothing on the other side
    PriceOutOfRange,  // v2 only: price outside the configured range
    IdOutOfRange,     // v2 only: id >= configured max_order_id
    BookFull,         // v2 only: order pool has no free slot
};

struct Accepted  { OrderId id;                            bool operator==(const Accepted&)  const = default; };
struct Trade     { OrderId resting; OrderId aggressor; Price price; Quantity qty;
                                                          bool operator==(const Trade&)     const = default; };
struct Cancelled { OrderId id; Quantity remaining;        bool operator==(const Cancelled&) const = default; };
struct Modified  { OrderId id; Price price; Quantity qty; bool operator==(const Modified&)  const = default; };
struct Rejected  { OrderId id; RejectReason reason;       bool operator==(const Rejected&)  const = default; };

using Event = std::variant<Accepted, Trade, Cancelled, Modified, Rejected>;

}  // namespace ob
