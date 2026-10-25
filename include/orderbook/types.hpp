#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ob {

// Strong types: a Price can't be passed where a Quantity is expected.
struct Price    { std::int64_t  ticks{}; auto operator<=>(const Price&)    const = default; };
struct Quantity { std::uint64_t value{}; auto operator<=>(const Quantity&) const = default; };
struct OrderId  { std::uint64_t value{}; auto operator<=>(const OrderId&)  const = default; };

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

}  // namespace ob

namespace std {
template <>
struct hash<ob::OrderId> {
    size_t operator()(const ob::OrderId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
}  // namespace std
