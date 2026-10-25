#include "itch/itch.hpp"
#include <cstring>

namespace itch {
namespace {

// Big-endian field readers. Reading byte by byte avoids alignment and
// strict-aliasing problems (never cast the buffer to a struct).
std::uint16_t u16(std::span<const std::byte> b, std::size_t o) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(b[o]) << 8) |
                                       std::to_integer<std::uint16_t>(b[o + 1]));
}
std::uint32_t u32(std::span<const std::byte> b, std::size_t o) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(b[o + i]);
    return v;
}
std::uint64_t u64(std::span<const std::byte> b, std::size_t o) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(b[o + i]);
    return v;
}
Symbol sym(std::span<const std::byte> b, std::size_t o) {
    Symbol s{};
    std::memcpy(s.data(), b.data() + o, 8);
    return s;
}

// Every message starts with: type(1) locate(2) tracking(2) timestamp(6) = 11 bytes.
constexpr std::size_t kHeader = 11;

}  // namespace

std::expected<Message, DecodeError> decode(std::span<const std::byte> m) {
    if (m.empty()) return std::unexpected(DecodeError::Empty);
    const char type = static_cast<char>(m[0]);

    auto need = [&](std::size_t len) { return m.size() >= len; };
    const auto truncated = std::unexpected(DecodeError::Truncated);

    switch (type) {
        case 'R':   // Stock Directory, 39 bytes
            if (!need(39)) return truncated;
            return StockDirectory{u16(m, 1), sym(m, kHeader)};
        case 'A':   // Add Order, 36 bytes
        case 'F':   // Add Order with MPID attribution, 40 bytes (extra field ignored)
            if (!need(type == 'A' ? 36 : 40)) return truncated;
            return AddOrder{u16(m, 1), u64(m, 11), static_cast<char>(m[19]),
                            u32(m, 20), sym(m, 24), u32(m, 32)};
        case 'E':   // Order Executed, 31 bytes
        case 'C':   // Order Executed With Price, 36 bytes (price ignored for the book)
            if (!need(type == 'E' ? 31 : 36)) return truncated;
            return OrderExecuted{u16(m, 1), u64(m, 11), u32(m, 19)};
        case 'X':   // Order Cancel (partial), 23 bytes
            if (!need(23)) return truncated;
            return OrderCancel{u16(m, 1), u64(m, 11), u32(m, 19)};
        case 'D':   // Order Delete, 19 bytes
            if (!need(19)) return truncated;
            return OrderDelete{u16(m, 1), u64(m, 11)};
        case 'U':   // Order Replace, 35 bytes
            if (!need(35)) return truncated;
            return OrderReplace{u16(m, 1), u64(m, 11), u64(m, 19), u32(m, 27), u32(m, 31)};
        default:
            return std::unexpected(DecodeError::Ignored);
    }
}

FileReader::FileReader(const std::string& path)
    : in_(path, std::ios::binary), buf_(1 << 20) {
    ok_ = static_cast<bool>(in_);
}

// Make sure at least `need` unread bytes are in the buffer.
bool FileReader::fill(std::size_t need) {
    if (end_ - pos_ >= need) return true;
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);   // keep the unread tail
    end_ -= pos_;
    pos_ = 0;
    while (end_ < need && in_) {
        in_.read(reinterpret_cast<char*>(buf_.data() + end_),
                 static_cast<std::streamsize>(buf_.size() - end_));
        end_ += static_cast<std::size_t>(in_.gcount());
    }
    return end_ >= need;
}

bool FileReader::next(std::span<const std::byte>& msg) {
    if (!fill(2)) {
        truncated_ = end_ != pos_;
        return false;
    }
    const std::size_t len = u16(std::span<const std::byte>(buf_.data() + pos_, 2), 0);
    if (!fill(2 + len)) {
        truncated_ = true;
        return false;
    }
    msg = std::span<const std::byte>(buf_.data() + pos_ + 2, len);
    pos_ += 2 + len;
    return true;
}

std::string to_string(const Symbol& s) {
    std::string out(s.data(), s.size());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace itch
