#ifndef HOTPATH_WIRE_OUCH_HPP
#define HOTPATH_WIRE_OUCH_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/wire/fields.hpp"

namespace hotpath::wire::ouch {

inline constexpr std::uint32_t kPriceScale = 10'000;
inline constexpr std::uint32_t kMaxPrice = 0x7735'939CU;
inline constexpr std::uint32_t kMarketPrice = 0x7FFF'FFFFU;
inline constexpr std::uint32_t kTifImmediateOrCancel = 0;
inline constexpr std::uint32_t kTifExtendedTradingClose = 99'996;
inline constexpr std::uint32_t kTifMarketHours = 99'998;
inline constexpr std::uint32_t kTifSystemHours = 99'999;

using Token = Alpha<14>;

struct EnterOrder {
    static constexpr char kType = 'O';
    char type;
    Token token;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
    U32 time_in_force;
    Alpha<4> firm;
    char display;
    char capacity;
    char intermarket_sweep;
    U32 minimum_quantity;
    char cross_type;
    char customer_type;
};

struct ReplaceOrder {
    static constexpr char kType = 'U';
    char type;
    Token existing_token;
    Token replacement_token;
    U32 shares;
    U32 price;
    U32 time_in_force;
    char display;
    char intermarket_sweep;
    U32 minimum_quantity;
};

struct CancelOrder {
    static constexpr char kType = 'X';
    char type;
    Token token;
    U32 shares;
};

struct ModifyOrder {
    static constexpr char kType = 'M';
    char type;
    Token token;
    char side;
    U32 shares;
};

struct SystemEvent {
    static constexpr char kType = 'S';
    char type;
    U64 timestamp_ns;
    char event_code;
};

struct Accepted {
    static constexpr char kType = 'A';
    char type;
    U64 timestamp_ns;
    Token token;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
    U32 time_in_force;
    Alpha<4> firm;
    char display;
    U64 order_ref;
    char capacity;
    char intermarket_sweep;
    U32 minimum_quantity;
    char cross_type;
    char order_state;
    char bbo_weight;
};

struct Replaced {
    static constexpr char kType = 'U';
    char type;
    U64 timestamp_ns;
    Token replacement_token;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
    U32 time_in_force;
    Alpha<4> firm;
    char display;
    U64 order_ref;
    char capacity;
    char intermarket_sweep;
    U32 minimum_quantity;
    char cross_type;
    char order_state;
    Token previous_token;
    char bbo_weight;
};

struct Canceled {
    static constexpr char kType = 'C';
    char type;
    U64 timestamp_ns;
    Token token;
    U32 decrement_shares;
    char reason;
};

struct Executed {
    static constexpr char kType = 'E';
    char type;
    U64 timestamp_ns;
    Token token;
    U32 executed_shares;
    U32 execution_price;
    char liquidity_flag;
    U64 match_number;
};

struct Rejected {
    static constexpr char kType = 'J';
    char type;
    U64 timestamp_ns;
    Token token;
    char reason;
};

struct AiqCanceled {
    static constexpr char kType = 'D';
    char type;
    U64 timestamp_ns;
    Token token;
    U32 decrement_shares;
    char reason;
    U32 quantity_prevented;
    U32 execution_price;
    char liquidity_flag;
    char aiq_strategy;
};

struct BrokenTrade {
    static constexpr char kType = 'B';
    char type;
    U64 timestamp_ns;
    Token token;
    U64 match_number;
    char reason;
};

struct ExecutedWithReferencePrice {
    static constexpr char kType = 'G';
    char type;
    U64 timestamp_ns;
    Token token;
    U32 executed_shares;
    U32 execution_price;
    char liquidity_flag;
    U64 match_number;
    U32 reference_price;
    char reference_price_type;
};

struct CancelPending {
    static constexpr char kType = 'P';
    char type;
    U64 timestamp_ns;
    Token token;
};

struct CancelReject {
    static constexpr char kType = 'I';
    char type;
    U64 timestamp_ns;
    Token token;
};

struct PriorityUpdate {
    static constexpr char kType = 'T';
    char type;
    U64 timestamp_ns;
    Token token;
    U32 price;
    char display;
    U64 order_ref;
};

struct Modified {
    static constexpr char kType = 'M';
    char type;
    U64 timestamp_ns;
    Token token;
    char side;
    U32 shares;
};

static_assert(WireMessage<EnterOrder> && sizeof(EnterOrder) == 49);
static_assert(WireMessage<ReplaceOrder> && sizeof(ReplaceOrder) == 47);
static_assert(WireMessage<CancelOrder> && sizeof(CancelOrder) == 19);
static_assert(WireMessage<ModifyOrder> && sizeof(ModifyOrder) == 20);
static_assert(WireMessage<SystemEvent> && sizeof(SystemEvent) == 10);
static_assert(WireMessage<Accepted> && sizeof(Accepted) == 66);
static_assert(WireMessage<Replaced> && sizeof(Replaced) == 80);
static_assert(WireMessage<Canceled> && sizeof(Canceled) == 28);
static_assert(WireMessage<Executed> && sizeof(Executed) == 40);
static_assert(WireMessage<Rejected> && sizeof(Rejected) == 24);
static_assert(WireMessage<AiqCanceled> && sizeof(AiqCanceled) == 38);
static_assert(WireMessage<BrokenTrade> && sizeof(BrokenTrade) == 32);
static_assert(WireMessage<ExecutedWithReferencePrice> && sizeof(ExecutedWithReferencePrice) == 45);
static_assert(WireMessage<CancelPending> && sizeof(CancelPending) == 23);
static_assert(WireMessage<CancelReject> && sizeof(CancelReject) == 23);
static_assert(WireMessage<PriorityUpdate> && sizeof(PriorityUpdate) == 36);
static_assert(WireMessage<Modified> && sizeof(Modified) == 28);

template <WireMessage M>
[[nodiscard]] constexpr M make() noexcept {
    M msg{};
    msg.type = M::kType;
    return msg;
}

namespace detail {

template <WireMessage M, typename H>
[[nodiscard]] [[gnu::always_inline]] inline Parse deliver(std::span<const std::byte> msg,
                                                          H& handler) noexcept {
    if (msg.size() < sizeof(M)) {
        return Parse::Truncated;
    }
    const M& view = *reinterpret_cast<const M*>(msg.data());
    if constexpr (requires { handler.on(view); }) {
        handler.on(view);
    }
    return Parse::Ok;
}

} // namespace detail

template <typename H>
[[nodiscard]] inline Parse dispatch_inbound(std::span<const std::byte> msg, H& handler) noexcept {
    if (msg.empty()) {
        return Parse::Truncated;
    }
    switch (static_cast<char>(msg[0])) {
        case EnterOrder::kType:   return detail::deliver<EnterOrder>(msg, handler);
        case ReplaceOrder::kType: return detail::deliver<ReplaceOrder>(msg, handler);
        case CancelOrder::kType:  return detail::deliver<CancelOrder>(msg, handler);
        case ModifyOrder::kType:  return detail::deliver<ModifyOrder>(msg, handler);
        default:                  return Parse::Unknown;
    }
}

template <typename H>
[[nodiscard]] inline Parse dispatch_outbound(std::span<const std::byte> msg, H& handler) noexcept {
    if (msg.empty()) {
        return Parse::Truncated;
    }
    switch (static_cast<char>(msg[0])) {
        case Executed::kType:                   return detail::deliver<Executed>(msg, handler);
        case Accepted::kType:                   return detail::deliver<Accepted>(msg, handler);
        case Canceled::kType:                   return detail::deliver<Canceled>(msg, handler);
        case Replaced::kType:                   return detail::deliver<Replaced>(msg, handler);
        case Rejected::kType:                   return detail::deliver<Rejected>(msg, handler);
        case ExecutedWithReferencePrice::kType: return detail::deliver<ExecutedWithReferencePrice>(msg, handler);
        case AiqCanceled::kType:                return detail::deliver<AiqCanceled>(msg, handler);
        case BrokenTrade::kType:                return detail::deliver<BrokenTrade>(msg, handler);
        case CancelPending::kType:              return detail::deliver<CancelPending>(msg, handler);
        case CancelReject::kType:               return detail::deliver<CancelReject>(msg, handler);
        case PriorityUpdate::kType:             return detail::deliver<PriorityUpdate>(msg, handler);
        case Modified::kType:                   return detail::deliver<Modified>(msg, handler);
        case SystemEvent::kType:                return detail::deliver<SystemEvent>(msg, handler);
        default:                                return Parse::Unknown;
    }
}

} // namespace hotpath::wire::ouch

#endif // HOTPATH_WIRE_OUCH_HPP
