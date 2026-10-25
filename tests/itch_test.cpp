#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "itch/itch.hpp"
#include "itch/replay.hpp"

namespace {

// Builds ITCH messages byte by byte, big-endian, exactly as the spec lays them out.
struct Bytes {
    std::vector<std::byte> b;
    Bytes& c(char x) { b.push_back(static_cast<std::byte>(x)); return *this; }
    Bytes& n(std::uint64_t v, int width) {
        for (int i = width - 1; i >= 0; --i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    Bytes& s(const char* sym) {
        std::string t(sym);
        t.resize(8, ' ');
        for (char x : t) c(x);
        return *this;
    }
    Bytes& header(char type, std::uint16_t locate) { return c(type).n(locate, 2).n(0, 2).n(123, 6); }
    std::span<const std::byte> span() const { return b; }
};

Bytes add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t sh, const char* sym, std::uint32_t px) {
    Bytes m; m.header('A', loc).n(ref, 8).c(side).n(sh, 4).s(sym).n(px, 4); return m;
}
Bytes directory(std::uint16_t loc, const char* sym) {
    Bytes m; m.header('R', loc).s(sym);
    for (int i = 0; i < 20; ++i) m.c('0');   // remaining 20 bytes of the 39-byte message
    return m;
}

}  // namespace

TEST(ItchDecode, AddOrder) {
    auto m = add(13, 0x0102030405060708ull, 'B', 250, "AAPL", 1'501'200);
    ASSERT_EQ(m.b.size(), 36u);
    auto r = itch::decode(m.span());
    ASSERT_TRUE(r.has_value());
    auto a = std::get<itch::AddOrder>(*r);
    EXPECT_EQ(a.locate, 13);
    EXPECT_EQ(a.ref, 0x0102030405060708ull);
    EXPECT_EQ(a.side, 'B');
    EXPECT_EQ(a.shares, 250u);
    EXPECT_EQ(a.price, 1'501'200u);        // $150.12 with 4 implied decimals
    EXPECT_EQ(itch::to_string(a.stock), "AAPL");
}

TEST(ItchDecode, StockDirectory) {
    auto m = directory(7, "MSFT");
    ASSERT_EQ(m.b.size(), 39u);
    auto r = itch::decode(m.span());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(std::get<itch::StockDirectory>(*r).locate, 7);
}

TEST(ItchDecode, ExecuteCancelDeleteReplace) {
    Bytes e; e.header('E', 1).n(99, 8).n(40, 4).n(5, 8);
    Bytes x; x.header('X', 1).n(99, 8).n(10, 4);
    Bytes d; d.header('D', 1).n(99, 8);
    Bytes u; u.header('U', 1).n(99, 8).n(100, 8).n(70, 4).n(1'500'000, 4);
    ASSERT_EQ(e.b.size(), 31u);
    ASSERT_EQ(x.b.size(), 23u);
    ASSERT_EQ(d.b.size(), 19u);
    ASSERT_EQ(u.b.size(), 35u);
    EXPECT_EQ(std::get<itch::OrderExecuted>(*itch::decode(e.span())).shares, 40u);
    EXPECT_EQ(std::get<itch::OrderCancel>(*itch::decode(x.span())).shares, 10u);
    EXPECT_EQ(std::get<itch::OrderDelete>(*itch::decode(d.span())).ref, 99u);
    auto rep = std::get<itch::OrderReplace>(*itch::decode(u.span()));
    EXPECT_EQ(rep.old_ref, 99u);
    EXPECT_EQ(rep.new_ref, 100u);
    EXPECT_EQ(rep.price, 1'500'000u);
}

TEST(ItchDecode, TruncatedAndIgnored) {
    auto m = add(1, 1, 'S', 1, "X", 1);
    m.b.resize(20);
    EXPECT_EQ(itch::decode(m.span()).error(), itch::DecodeError::Truncated);
    Bytes sys; sys.header('S', 0).c('O');
    EXPECT_EQ(itch::decode(sys.span()).error(), itch::DecodeError::Ignored);
    EXPECT_EQ(itch::decode({}).error(), itch::DecodeError::Empty);
}

TEST(ItchFile, ReadsFramesAndDetectsTruncatedTail) {
    const std::string path = ::testing::TempDir() + "itch_test.bin";
    {
        std::ofstream f(path, std::ios::binary);
        const std::vector<Bytes> msgs{directory(5, "TEST"), add(5, 1, 'B', 10, "TEST", 100)};
        for (const auto& m : msgs) {
            Bytes len; len.n(m.b.size(), 2);
            f.write(reinterpret_cast<const char*>(len.b.data()), 2);
            f.write(reinterpret_cast<const char*>(m.b.data()), static_cast<std::streamsize>(m.b.size()));
        }
        const char tail[] = {0, 36, 'A', 0, 0};   // says 36 bytes, file ends after 3
        f.write(tail, sizeof(tail));
    }
    itch::FileReader in(path);
    ASSERT_TRUE(in.ok());
    std::span<const std::byte> msg;
    int n = 0;
    while (in.next(msg)) ++n;
    EXPECT_EQ(n, 2);
    EXPECT_TRUE(in.truncated_tail());
    std::remove(path.c_str());
}

TEST(ItchReplay, BuildsDenseOpsForOneSymbol) {
    itch::ReplayBuilder rb("AAPL");
    auto feed = [&](const Bytes& m) { auto r = itch::decode(m.span()); rb.count(r); if (r) rb.feed(*r); };
    feed(directory(13, "AAPL"));
    feed(directory(14, "MSFT"));
    feed(add(13, 5'000'000'001ull, 'B', 100, "AAPL", 1'500'000));
    feed(add(14, 5'000'000'002ull, 'S', 100, "MSFT", 3'000'000));   // other symbol: skipped
    Bytes e; e.header('E', 13).n(5'000'000'001ull, 8).n(30, 4).n(1, 8);
    feed(e);
    Bytes u; u.header('U', 13).n(5'000'000'001ull, 8).n(5'000'000'003ull, 8).n(50, 4).n(1'499'900, 4);
    feed(u);

    ASSERT_TRUE(rb.found_symbol());
    const auto& ops = rb.ops();
    ASSERT_EQ(ops.size(), 4u);                                   // add, reduce, cancel, add
    EXPECT_EQ(ops[0].kind, itch::ReplayOp::Kind::Add);
    EXPECT_EQ(ops[0].order.id, ob::OrderId{1});                  // dense id, not the ITCH ref
    EXPECT_EQ(ops[1].kind, itch::ReplayOp::Kind::Reduce);
    EXPECT_EQ(ops[1].order.remaining, ob::Quantity{70});
    EXPECT_EQ(ops[2].kind, itch::ReplayOp::Kind::Cancel);
    EXPECT_EQ(ops[3].order.id, ob::OrderId{2});
    EXPECT_EQ(ops[3].order.side, ob::Side::Buy);                 // replace keeps the side
    EXPECT_EQ(rb.stats().unknown_refs, 0u);
}
