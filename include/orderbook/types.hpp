#pragma once
#include <compare>
#include <cstdint>

namespace ob {

struct Price    { std::int64_t  ticks{}; auto operator<=>(const Price&)    const = default; };
struct Quantity { std::uint64_t value{}; auto operator<=>(const Quantity&) const = default; };
struct OrderId  { std::uint64_t value{}; auto operator<=>(const OrderId&)  const = default; };

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// TODO: std::hash<ob::OrderId> specialization (needed for unordered_map).

}  // namespace ob
