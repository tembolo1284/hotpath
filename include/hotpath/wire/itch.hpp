#ifndef HOTPATH_WIRE_ITCH_HPP
#define HOTPATH_WIRE_ITCH_HPP

#include <cstddef>
#include <span>

#include "hotpath/wire/fields.hpp"

namespace hotpath::wire::itch {

inline constexpr std::uint32_t kPriceScale = 10'000;
inline constexpr std::uint32_t kMaxPrice = 0x7735'9400U;

struct Header {
    char type;
    U16 stock_locate;
    U16 tracking_number;
    U48 timestamp_ns;
};

struct SystemEvent {
    static constexpr char kType = 'S';
    Header hdr;
    char event_code;
};

struct StockDirectory {
    static constexpr char kType = 'R';
    Header hdr;
    Alpha<8> stock;
    char market_category;
    char financial_status;
    U32 round_lot_size;
    char round_lots_only;
    char issue_classification;
    Alpha<2> issue_sub_type;
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    U32 etp_leverage_factor;
    char inverse_indicator;
};

struct StockTradingAction {
    static constexpr char kType = 'H';
    Header hdr;
    Alpha<8> stock;
    char trading_state;
    char reserved;
    Alpha<4> reason;
};

struct AddOrder {
    static constexpr char kType = 'A';
    Header hdr;
    U64 order_ref;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
};

struct AddOrderMpid {
    static constexpr char kType = 'F';
    Header hdr;
    U64 order_ref;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
    Alpha<4> attribution;
};

struct OrderExecuted {
    static constexpr char kType = 'E';
    Header hdr;
    U64 order_ref;
    U32 executed_shares;
    U64 match_number;
};

struct OrderExecutedWithPrice {
    static constexpr char kType = 'C';
    Header hdr;
    U64 order_ref;
    U32 executed_shares;
    U64 match_number;
    char printable;
    U32 execution_price;
};

struct OrderCancel {
    static constexpr char kType = 'X';
    Header hdr;
    U64 order_ref;
    U32 cancelled_shares;
};

struct OrderDelete {
    static constexpr char kType = 'D';
    Header hdr;
    U64 order_ref;
};

struct OrderReplace {
    static constexpr char kType = 'U';
    Header hdr;
    U64 original_order_ref;
    U64 new_order_ref;
    U32 shares;
    U32 price;
};

struct Trade {
    static constexpr char kType = 'P';
    Header hdr;
    U64 order_ref;
    char side;
    U32 shares;
    Alpha<8> stock;
    U32 price;
    U64 match_number;
};

struct BrokenTrade {
    static constexpr char kType = 'B';
    Header hdr;
    U64 match_number;
};

static_assert(WireMessage<Header> && sizeof(Header) == 11);
static_assert(WireMessage<SystemEvent> && sizeof(SystemEvent) == 12);
static_assert(WireMessage<StockDirectory> && sizeof(StockDirectory) == 39);
static_assert(WireMessage<StockTradingAction> && sizeof(StockTradingAction) == 25);
static_assert(WireMessage<AddOrder> && sizeof(AddOrder) == 36);
static_assert(WireMessage<AddOrderMpid> && sizeof(AddOrderMpid) == 40);
static_assert(WireMessage<OrderExecuted> && sizeof(OrderExecuted) == 31);
static_assert(WireMessage<OrderExecutedWithPrice> && sizeof(OrderExecutedWithPrice) == 36);
static_assert(WireMessage<OrderCancel> && sizeof(OrderCancel) == 23);
static_assert(WireMessage<OrderDelete> && sizeof(OrderDelete) == 19);
static_assert(WireMessage<OrderReplace> && sizeof(OrderReplace) == 35);
static_assert(WireMessage<Trade> && sizeof(Trade) == 44);
static_assert(WireMessage<BrokenTrade> && sizeof(BrokenTrade) == 19);

template <WireMessage M>
[[nodiscard]] constexpr M make() noexcept {
    M msg{};
    msg.hdr.type = M::kType;
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
[[nodiscard]] inline Parse dispatch(std::span<const std::byte> msg, H& handler) noexcept {
    if (msg.empty()) {
        return Parse::Truncated;
    }
    switch (static_cast<char>(msg[0])) {
        case AddOrder::kType:               return detail::deliver<AddOrder>(msg, handler);
        case AddOrderMpid::kType:           return detail::deliver<AddOrderMpid>(msg, handler);
        case OrderExecuted::kType:          return detail::deliver<OrderExecuted>(msg, handler);
        case OrderExecutedWithPrice::kType: return detail::deliver<OrderExecutedWithPrice>(msg, handler);
        case OrderCancel::kType:            return detail::deliver<OrderCancel>(msg, handler);
        case OrderDelete::kType:            return detail::deliver<OrderDelete>(msg, handler);
        case OrderReplace::kType:           return detail::deliver<OrderReplace>(msg, handler);
        case Trade::kType:                  return detail::deliver<Trade>(msg, handler);
        case SystemEvent::kType:            return detail::deliver<SystemEvent>(msg, handler);
        case StockDirectory::kType:         return detail::deliver<StockDirectory>(msg, handler);
        case StockTradingAction::kType:     return detail::deliver<StockTradingAction>(msg, handler);
        case BrokenTrade::kType:            return detail::deliver<BrokenTrade>(msg, handler);
        default:                            return Parse::Unknown;
    }
}

} // namespace hotpath::wire::itch

#endif // HOTPATH_WIRE_ITCH_HPP
