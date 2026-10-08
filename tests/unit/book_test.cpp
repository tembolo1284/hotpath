#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "hotpath/book/book_set.hpp"
#include "hotpath/book/level_side.hpp"
#include "hotpath/book/order_book.hpp"
#include "hotpath/book/order_map.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"

using namespace hotpath;

namespace {

class BookTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(region_.map(std::size_t{64} << 20, sys::PageKind::Small).ok());
        region_.prefault();
        arena_ = mem::Arena{region_};
    }

    mem::Region region_;
    mem::Arena arena_;
};

TEST_F(BookTest, OrderMapMatchesReferenceUnderChurn) {
    book::OrderMap map;
    ASSERT_EQ(map.init(arena_, 100).err(), EINVAL);
    ASSERT_TRUE(map.init(arena_, 1024).ok());

    std::unordered_map<std::uint64_t, std::uint32_t> model;
    std::mt19937_64 rng{7};
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t id = rng() % 1500U;
        const auto it = model.find(id);
        if (it == model.end()) {
            const auto qty = static_cast<std::uint32_t>(1U + rng() % 1000U);
            const book::OrderMap::Insert slot =
                map.insert(id, static_cast<std::uint32_t>(id), Side::Sell, qty);
            ASSERT_FALSE(slot.existed);
            if (model.size() >= map.max_load()) {
                ASSERT_EQ(slot.entry, nullptr);
            } else {
                ASSERT_NE(slot.entry, nullptr);
                model.emplace(id, qty);
            }
        } else {
            book::OrderEntry* entry = map.find(id);
            ASSERT_NE(entry, nullptr);
            ASSERT_EQ(entry->qty, it->second);
            ASSERT_EQ(entry->ticks(), id);
            ASSERT_EQ(entry->side(), Side::Sell);
            ASSERT_TRUE(map.insert(id, 1, Side::Buy, 1).existed);
            map.erase(entry);
            model.erase(it);
            ASSERT_EQ(map.find(id), nullptr);
        }
        ASSERT_EQ(map.size(), model.size());
    }
    for (const auto& [id, qty] : model) {
        book::OrderEntry* entry = map.find(id);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->qty, qty);
    }
}

TEST_F(BookTest, LevelSideOrdersBestFirstOnBothSides) {
    book::LevelSide<Side::Buy> bids;
    book::LevelSide<Side::Sell> asks;
    ASSERT_TRUE(bids.init(arena_, 64).ok());
    ASSERT_TRUE(asks.init(arena_, 64).ok());
    EXPECT_EQ(bids.best(), book::Level{});

    for (const std::uint32_t ticks : {1000U, 1020U, 990U, 1010U, 1020U, 400U}) {
        bids.add(ticks, 10);
        asks.add(ticks, 10);
    }
    ASSERT_EQ(bids.depth(), 5U);
    EXPECT_EQ(bids.best(), (book::Level{1020, 2, 20}));
    EXPECT_EQ(bids.at(1).ticks, 1010U);
    EXPECT_EQ(bids.at(4).ticks, 400U);
    EXPECT_EQ(asks.best(), (book::Level{400, 1, 10}));
    EXPECT_EQ(asks.at(4), (book::Level{1020, 2, 20}));

    bids.reduce(1020, 4, false);
    EXPECT_EQ(bids.best(), (book::Level{1020, 2, 16}));
    bids.reduce(1020, 6, true);
    bids.reduce(1020, 10, true);
    EXPECT_EQ(bids.best().ticks, 1010U);
    EXPECT_EQ(bids.depth(), 4U);
    EXPECT_EQ(bids.best().price(), Price{1010});
}

TEST_F(BookTest, LevelSideKeepsNearestLevelsWhenFull) {
    book::LevelSide<Side::Buy> bids;
    ASSERT_TRUE(bids.init(arena_, 4).ok());
    for (const std::uint32_t ticks : {100U, 101U, 102U, 103U}) {
        bids.add(ticks, 5);
    }
    EXPECT_FALSE(bids.has_horizon());

    bids.add(99, 5);
    EXPECT_TRUE(bids.has_horizon());
    EXPECT_EQ(bids.horizon_ticks(), 100U);
    EXPECT_EQ(bids.orders_beyond_horizon(), 1U);
    EXPECT_EQ(bids.depth(), 4U);

    bids.add(104, 7);
    EXPECT_EQ(bids.best(), (book::Level{104, 1, 7}));
    EXPECT_EQ(bids.at(3).ticks, 101U);
    EXPECT_EQ(bids.horizon_ticks(), 101U);
    EXPECT_EQ(bids.orders_beyond_horizon(), 2U);

    bids.reduce(100, 5, true);
    bids.reduce(99, 2, false);
    EXPECT_EQ(bids.orders_beyond_horizon(), 1U);
    bids.reduce(99, 3, true);
    EXPECT_EQ(bids.orders_beyond_horizon(), 0U);

    bids.add(100, 5);
    EXPECT_EQ(bids.depth(), 4U);
    EXPECT_EQ(bids.orders_beyond_horizon(), 1U);

    bids.add(102, 1);
    EXPECT_EQ(bids.at(2), (book::Level{102, 2, 6}));

    for (const std::uint32_t ticks : {101U, 103U, 104U}) {
        bids.reduce(ticks, ticks == 104U ? 7U : 5U, true);
    }
    bids.reduce(102, 5, true);
    EXPECT_FALSE(bids.degraded());
    bids.reduce(102, 1, true);
    EXPECT_EQ(bids.depth(), 0U);
    EXPECT_TRUE(bids.degraded());
}

TEST_F(BookTest, BookSetAppliesOrderLifecycle) {
    book::BookSet books;
    const book::BookConfig config{.max_symbols = 8, .max_orders = 1024, .levels_per_side = 32};
    ASSERT_LE(book::BookSet::bytes_for(config), arena_.remaining());
    ASSERT_TRUE(books.init(arena_, config).ok());
    const SymbolId sym{3};

    book::Update up = books.add(sym, OrderId{1}, Side::Buy, Qty{100}, Price{10'000});
    EXPECT_TRUE(up.ok());
    EXPECT_TRUE(up.top_changed);
    up = books.add(sym, OrderId{2}, Side::Buy, Qty{50}, Price{9'900});
    EXPECT_TRUE(up.ok());
    EXPECT_FALSE(up.top_changed);
    up = books.add(sym, OrderId{3}, Side::Sell, Qty{70}, Price{10'100});
    EXPECT_TRUE(up.top_changed);

    book::Top top = books.book(sym)->top();
    EXPECT_TRUE(top.two_sided());
    EXPECT_EQ(top.bid, (book::Level{10'000, 1, 100}));
    EXPECT_EQ(top.ask, (book::Level{10'100, 1, 70}));

    up = books.reduce(sym, OrderId{1}, Qty{40});
    EXPECT_TRUE(up.ok());
    EXPECT_TRUE(up.top_changed);
    EXPECT_EQ(books.book(sym)->top().bid.qty, 60U);
    EXPECT_EQ(books.reduce(sym, OrderId{1}, Qty{61}).result, book::Result::Invalid);

    up = books.reduce(sym, OrderId{1}, Qty{60});
    EXPECT_TRUE(up.top_changed);
    EXPECT_EQ(books.book(sym)->top().bid.ticks, 9'900U);
    EXPECT_EQ(books.order(OrderId{1}), nullptr);

    up = books.replace(sym, OrderId{3}, OrderId{4}, Qty{30}, Price{10'050});
    EXPECT_TRUE(up.ok());
    EXPECT_TRUE(up.top_changed);
    EXPECT_EQ(books.book(sym)->top().ask, (book::Level{10'050, 1, 30}));
    ASSERT_NE(books.order(OrderId{4}), nullptr);
    EXPECT_EQ(books.order(OrderId{4})->side(), Side::Sell);

    up = books.remove(sym, OrderId{4});
    EXPECT_TRUE(up.top_changed);
    EXPECT_FALSE(books.book(sym)->top().has_ask());
    EXPECT_EQ(books.open_orders(), 1U);

    EXPECT_EQ(books.add(sym, OrderId{2}, Side::Buy, Qty{1}, Price{1}).result,
              book::Result::DuplicateOrder);
    EXPECT_EQ(books.remove(sym, OrderId{99}).result, book::Result::UnknownOrder);
    EXPECT_EQ(books.reduce(sym, OrderId{99}, Qty{1}).result, book::Result::UnknownOrder);
    EXPECT_EQ(books.replace(sym, OrderId{99}, OrderId{5}, Qty{1}, Price{1}).result,
              book::Result::UnknownOrder);
    EXPECT_EQ(books.add(SymbolId{8}, OrderId{6}, Side::Buy, Qty{1}, Price{1}).result,
              book::Result::BadSymbol);
    EXPECT_EQ(books.add(sym, OrderId{6}, Side::Buy, Qty{0}, Price{1}).result,
              book::Result::Invalid);
    EXPECT_EQ(books.add(sym, OrderId{6}, Side::Buy, Qty{1}, Price{-1}).result,
              book::Result::Invalid);
    EXPECT_EQ(books.book(SymbolId{8}), nullptr);
    EXPECT_EQ(books.open_orders(), 1U);
}

struct ModelOrder {
    std::uint16_t symbol;
    Side side;
    std::uint32_t ticks;
    std::uint32_t qty;
};

struct ModelLevel {
    std::uint64_t qty{0};
    std::uint32_t orders{0};
};

struct ModelBook {
    std::map<std::uint32_t, ModelLevel, std::greater<>> bids;
    std::map<std::uint32_t, ModelLevel> asks;

    void add(const ModelOrder& o) {
        ModelLevel& level = o.side == Side::Buy ? bids[o.ticks] : asks[o.ticks];
        level.qty += o.qty;
        level.orders += 1;
    }

    void reduce(const ModelOrder& o, std::uint32_t qty, bool gone) {
        if (o.side == Side::Buy) {
            apply(bids, o.ticks, qty, gone);
        } else {
            apply(asks, o.ticks, qty, gone);
        }
    }

    template <typename M>
    static void apply(M& side, std::uint32_t ticks, std::uint32_t qty, bool gone) {
        const auto it = side.find(ticks);
        it->second.qty -= qty;
        if (gone) {
            it->second.orders -= 1;
        }
        if (it->second.orders == 0) {
            side.erase(it);
        }
    }

    [[nodiscard]] book::Top top() const {
        book::Top out;
        if (!bids.empty()) {
            out.bid = {bids.begin()->first, bids.begin()->second.orders, bids.begin()->second.qty};
        }
        if (!asks.empty()) {
            out.ask = {asks.begin()->first, asks.begin()->second.orders, asks.begin()->second.qty};
        }
        return out;
    }
};

template <typename SideT, typename M>
void expect_side_equal(const SideT& side, const M& model) {
    ASSERT_EQ(side.depth(), model.size());
    std::uint32_t depth = 0;
    for (const auto& [ticks, level] : model) {
        ASSERT_EQ(side.at(depth), (book::Level{ticks, level.orders, level.qty}));
        depth += 1;
    }
}

TEST_F(BookTest, BookSetMatchesReferenceModelUnderRandomFlow) {
    constexpr std::uint16_t kSymbols = 4;
    book::BookSet books;
    ASSERT_TRUE(books.init(arena_, {.max_symbols = kSymbols, .max_orders = 1U << 17,
                                    .levels_per_side = 512}).ok());

    std::vector<ModelBook> model(kSymbols);
    std::unordered_map<std::uint64_t, ModelOrder> orders;
    std::vector<std::uint64_t> live;
    std::mt19937_64 rng{42};
    std::uint64_t next_id = 1;

    for (int step = 0; step < 300'000; ++step) {
        const auto roll = rng() % 100U;
        bool expect_changed = false;
        book::Update up{};
        std::uint16_t touched = 0;

        if (roll < 45 || live.empty()) {
            const ModelOrder o{static_cast<std::uint16_t>(rng() % kSymbols),
                               (rng() & 1U) != 0 ? Side::Buy : Side::Sell,
                               static_cast<std::uint32_t>(9'900U + rng() % 200U),
                               static_cast<std::uint32_t>(1U + rng() % 500U)};
            const std::uint64_t id = next_id++;
            touched = o.symbol;
            const book::Top before = model[o.symbol].top();
            model[o.symbol].add(o);
            expect_changed = before != model[o.symbol].top();
            orders.emplace(id, o);
            live.push_back(id);
            up = books.add(SymbolId{o.symbol}, OrderId{id}, o.side, Qty{o.qty},
                           Price{static_cast<std::int64_t>(o.ticks)});
        } else {
            const std::size_t pick = rng() % live.size();
            const std::uint64_t id = live[pick];
            ModelOrder& o = orders.at(id);
            touched = o.symbol;
            const book::Top before = model[o.symbol].top();

            if (roll < 65) {
                const auto qty = static_cast<std::uint32_t>(1U + rng() % o.qty);
                const bool gone = qty == o.qty;
                model[o.symbol].reduce(o, qty, gone);
                up = books.reduce(SymbolId{o.symbol}, OrderId{id}, Qty{qty});
                o.qty -= qty;
                if (gone) {
                    orders.erase(id);
                    live[pick] = live.back();
                    live.pop_back();
                }
            } else if (roll < 85) {
                model[o.symbol].reduce(o, o.qty, true);
                up = books.remove(SymbolId{o.symbol}, OrderId{id});
                orders.erase(id);
                live[pick] = live.back();
                live.pop_back();
            } else {
                model[o.symbol].reduce(o, o.qty, true);
                ModelOrder fresh = o;
                fresh.ticks = static_cast<std::uint32_t>(9'900U + rng() % 200U);
                fresh.qty = static_cast<std::uint32_t>(1U + rng() % 500U);
                model[fresh.symbol].add(fresh);
                const std::uint64_t new_id = next_id++;
                up = books.replace(SymbolId{fresh.symbol}, OrderId{id}, OrderId{new_id},
                                   Qty{fresh.qty}, Price{static_cast<std::int64_t>(fresh.ticks)});
                orders.erase(id);
                orders.emplace(new_id, fresh);
                live[pick] = new_id;
            }
            expect_changed = before != model[touched].top();
        }

        ASSERT_TRUE(up.ok()) << "step " << step;
        ASSERT_EQ(up.top_changed, expect_changed) << "step " << step;
        ASSERT_EQ(books.book(SymbolId{touched})->top(), model[touched].top()) << "step " << step;
        ASSERT_EQ(books.open_orders(), orders.size());

        if (step % 997 == 0) {
            for (std::uint16_t s = 0; s < kSymbols; ++s) {
                expect_side_equal(books.book(SymbolId{s})->bids(), model[s].bids);
                expect_side_equal(books.book(SymbolId{s})->asks(), model[s].asks);
            }
        }
    }
    for (std::uint16_t s = 0; s < kSymbols; ++s) {
        expect_side_equal(books.book(SymbolId{s})->bids(), model[s].bids);
        expect_side_equal(books.book(SymbolId{s})->asks(), model[s].asks);
        EXPECT_FALSE(books.book(SymbolId{s})->bids().has_horizon());
    }
}

} // namespace
