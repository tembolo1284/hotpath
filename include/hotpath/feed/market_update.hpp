#ifndef HOTPATH_FEED_MARKET_UPDATE_HPP
#define HOTPATH_FEED_MARKET_UPDATE_HPP

#include <concepts>
#include <cstdint>

#include "hotpath/core/types.hpp"

namespace hotpath::feed {

enum class Kind : std::uint8_t {
    TopOfBook = 1,
    Trade = 2,
    TradingStatus = 3,
    FeedStatus = 4,
};

inline constexpr std::uint8_t kFlagStale = 0x01;
inline constexpr std::uint8_t kFlagLossy = 0x02;
inline constexpr std::uint8_t kFlagDegraded = 0x04;
inline constexpr std::uint8_t kFlagHalted = 0x08;

inline constexpr std::uint16_t kNoSymbol = 0xFFFF;

struct alignas(kCacheLine) MarketUpdate {
    std::uint64_t rx_tsc{0};
    std::uint64_t exchange_ns{0};
    std::uint64_t sequence{0};
    std::uint64_t bid_qty{0};
    std::uint64_t ask_qty{0};
    std::uint32_t bid_ticks{0};
    std::uint32_t ask_ticks{0};
    std::uint32_t last_ticks{0};
    std::uint32_t last_qty{0};
    SymbolId symbol{kNoSymbol};
    Kind kind{Kind::TopOfBook};
    std::uint8_t flags{0};

    [[nodiscard]] constexpr bool has_bid() const noexcept { return bid_qty != 0; }
    [[nodiscard]] constexpr bool has_ask() const noexcept { return ask_qty != 0; }
    [[nodiscard]] constexpr bool tradable() const noexcept {
        return (flags & (kFlagStale | kFlagLossy | kFlagDegraded | kFlagHalted)) == 0;
    }
};

static_assert(ShmSafe<MarketUpdate>);
static_assert(sizeof(MarketUpdate) == kCacheLine && alignof(MarketUpdate) == kCacheLine);

template <typename S>
concept UpdateSink = requires(S& sink) {
    { sink.claim() } -> std::same_as<MarketUpdate*>;
    sink.publish();
};

} // namespace hotpath::feed

#endif // HOTPATH_FEED_MARKET_UPDATE_HPP
