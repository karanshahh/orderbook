#pragma once
// Nasdaq TotalView-ITCH 5.0 decoder: only the messages needed to rebuild an
// order book. Fields are big-endian; prices have 4 implied decimals.
// Spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace itch {

using Symbol = std::array<char, 8>;   // space-padded, not null-terminated

struct StockDirectory { std::uint16_t locate; Symbol stock; std::uint64_t ts = 0; };                         // 'R'
struct AddOrder       { std::uint16_t locate; std::uint64_t ref; char side;              // 'A', 'F'
                        std::uint32_t shares; Symbol stock; std::uint32_t price; std::uint64_t ts = 0; };
struct OrderExecuted  { std::uint16_t locate; std::uint64_t ref; std::uint32_t shares; std::uint64_t ts = 0; }; // 'E', 'C'
struct OrderCancel    { std::uint16_t locate; std::uint64_t ref; std::uint32_t shares; std::uint64_t ts = 0; }; // 'X'
struct OrderDelete    { std::uint16_t locate; std::uint64_t ref; std::uint64_t ts = 0; };                       // 'D'
struct OrderReplace   { std::uint16_t locate; std::uint64_t old_ref; std::uint64_t new_ref; // 'U'
                        std::uint32_t shares; std::uint32_t price; std::uint64_t ts = 0; };

using Message = std::variant<StockDirectory, AddOrder, OrderExecuted, OrderCancel,
                             OrderDelete, OrderReplace>;

enum class DecodeError {
    Ignored,     // a valid message type we don't need (trades, system events, ...)
    Truncated,   // shorter than the spec says for its type
    Empty,       // zero-length message
};

// Decode one message body (without its 2-byte length prefix).
std::expected<Message, DecodeError> decode(std::span<const std::byte> msg);

// Reads a BinaryFILE: each message is a 2-byte big-endian length, then the body.
// Uncompressed files only (gunzip first).
class FileReader {
public:
    explicit FileReader(const std::string& path);
    bool ok() const { return ok_; }

    // Next message body, or false at end of file.
    bool next(std::span<const std::byte>& msg);
    bool truncated_tail() const { return truncated_; }   // file ended mid-message

private:
    bool fill(std::size_t need);

    std::ifstream          in_;
    std::vector<std::byte> buf_;
    std::size_t            pos_ = 0, end_ = 0;
    bool                   ok_ = false, truncated_ = false;
};

std::string to_string(const Symbol& s);   // trims trailing spaces

}  // namespace itch
