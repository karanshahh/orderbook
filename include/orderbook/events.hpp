#pragma once
#include <variant>
#include "orderbook/types.hpp"

namespace ob {

enum class RejectReason { DuplicateId, ZeroQuantity, UnknownId, NoLiquidity };

struct Accepted  { OrderId id; };
struct Trade     { OrderId resting; OrderId aggressor; Price price; Quantity qty; };
struct Cancelled { OrderId id; Quantity remaining; };
struct Rejected  { OrderId id; RejectReason reason; };

using Event = std::variant<Accepted, Trade, Cancelled, Rejected>;

}  // namespace ob
