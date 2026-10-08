#ifndef HOTPATH_GATEWAY_ORDER_MESSAGES_HPP
#define HOTPATH_GATEWAY_ORDER_MESSAGES_HPP

#include <concepts>
#include <cstdint>

#include "hotpath/core/types.hpp"

namespace hotpath::gateway {

enum class RequestKind : std::uint8_t {
    New = 1,
    Cancel = 2,
    Replace = 3,
};

struct alignas(kCacheLine) OrderRequest {
    std::uint64_t client_id{0};
    std::uint64_t target_id{0};
    std::uint64_t origin_tsc{0};
    std::uint64_t decision_tsc{0};
    std::uint32_t qty{0};
    std::uint32_t price_ticks{0};
    std::uint32_t time_in_force{0};
    SymbolId symbol{};
    RequestKind kind{RequestKind::New};
    Side side{Side::Buy};
};

enum class ReportKind : std::uint8_t {
    Accepted = 1,
    Rejected = 2,
    Canceled = 3,
    Executed = 4,
    Replaced = 5,
    CancelPending = 6,
    CancelRejected = 7,
    TradeBroken = 8,
    SessionUp = 9,
    SessionDown = 10,
};

enum class RejectSource : std::uint8_t {
    None = 0,
    Venue = 1,
    Risk = 2,
    NotConnected = 3,
    Backpressure = 4,
    BadRequest = 5,
};

struct alignas(kCacheLine) ExecutionReport {
    std::uint64_t client_id{0};
    std::uint64_t related_id{0};
    std::uint64_t venue_ns{0};
    std::uint64_t rx_tsc{0};
    std::uint64_t order_ref{0};
    std::uint64_t match_number{0};
    std::uint32_t qty{0};
    std::uint32_t price_ticks{0};
    ReportKind kind{ReportKind::Accepted};
    RejectSource source{RejectSource::None};
    char reason{0};
    char liquidity{0};
};

static_assert(ShmSafe<OrderRequest> && sizeof(OrderRequest) == kCacheLine);
static_assert(ShmSafe<ExecutionReport> && sizeof(ExecutionReport) == kCacheLine);

template <typename S>
concept RequestSink = requires(S& sink) {
    { sink.claim() } -> std::same_as<OrderRequest*>;
    sink.publish();
};

template <typename S>
concept RequestSource = requires(S& source) {
    { source.peek() } -> std::same_as<const OrderRequest*>;
    source.consume();
};

template <typename S>
concept ReportSink = requires(S& sink) {
    { sink.claim() } -> std::same_as<ExecutionReport*>;
    sink.publish();
};

template <typename S>
concept ReportSource = requires(S& source) {
    { source.peek() } -> std::same_as<const ExecutionReport*>;
    source.consume();
};

} // namespace hotpath::gateway

#endif // HOTPATH_GATEWAY_ORDER_MESSAGES_HPP
