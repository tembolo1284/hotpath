#ifndef HOTPATH_STRATEGY_ORDER_TABLE_HPP
#define HOTPATH_STRATEGY_ORDER_TABLE_HPP

#include <bit>
#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::strategy {

struct OrderRecord {
    std::uint64_t id{0};
    std::uint32_t qty{0};
    std::uint32_t filled{0};
    std::uint32_t canceled{0};
    std::uint32_t price_ticks{0};
    SymbolId symbol{};
    Side side{Side::Buy};
    bool open{false};

    [[nodiscard]] constexpr std::uint32_t leaves() const noexcept {
        return qty - filled - canceled;
    }
};

struct Applied {
    bool known{false};
    bool done{false};
    SymbolId symbol{};
};

class OrderTable {
public:
    [[nodiscard]] Status init(mem::Arena& arena, std::uint32_t capacity,
                              std::uint16_t symbols) noexcept {
        HOTPATH_ASSERT(records_ == nullptr);
        if (capacity == 0 || !std::has_single_bit(capacity) || symbols == 0) {
            return Status{EINVAL};
        }
        OrderRecord* records = arena.allocate_array<OrderRecord>(capacity);
        std::int64_t* positions = arena.allocate_array<std::int64_t>(symbols);
        std::int64_t* cash = arena.allocate_array<std::int64_t>(symbols);
        if (records == nullptr || positions == nullptr || cash == nullptr) {
            return Status{ENOMEM};
        }
        records_ = records;
        positions_ = positions;
        cash_ = cash;
        mask_ = capacity - 1U;
        symbols_ = symbols;
        return Status{};
    }

    [[nodiscard]] bool can_insert(std::uint64_t id) const noexcept {
        return !records_[id & mask_].open;
    }

    [[nodiscard]] OrderRecord* insert(std::uint64_t id, SymbolId symbol, Side side,
                                      std::uint32_t qty, std::uint32_t price_ticks) noexcept {
        OrderRecord& slot = records_[id & mask_];
        if (slot.open || symbol.raw() >= symbols_ || qty == 0) {
            return nullptr;
        }
        slot = OrderRecord{id, qty, 0, 0, price_ticks, symbol, side, true};
        open_ += 1U;
        return &slot;
    }

    [[nodiscard]] OrderRecord* find(std::uint64_t id) noexcept {
        OrderRecord& slot = records_[id & mask_];
        return slot.open && slot.id == id ? &slot : nullptr;
    }

    Applied apply(const gateway::ExecutionReport& report) noexcept {
        using gateway::ReportKind;
        if (report.kind == ReportKind::Replaced) {
            return replaced(report);
        }
        OrderRecord* record = find(report.client_id);
        if (record == nullptr) {
            return Applied{};
        }
        const SymbolId symbol = record->symbol;

        switch (report.kind) {
            case ReportKind::Executed: {
                const std::uint32_t qty =
                    report.qty < record->leaves() ? report.qty : record->leaves();
                const auto signed_qty = static_cast<std::int64_t>(qty);
                const std::int64_t notional = signed_qty * static_cast<std::int64_t>(report.price_ticks);
                record->filled += qty;
                if (record->side == Side::Buy) {
                    positions_[symbol.raw()] += signed_qty;
                    cash_[symbol.raw()] -= notional;
                } else {
                    positions_[symbol.raw()] -= signed_qty;
                    cash_[symbol.raw()] += notional;
                }
                break;
            }
            case ReportKind::Canceled:
                record->canceled += report.qty < record->leaves() ? report.qty : record->leaves();
                break;
            case ReportKind::Rejected:
                record->canceled += record->leaves();
                break;
            case ReportKind::Accepted:
                if (report.reason == 'D') {
                    record->canceled += record->leaves();
                }
                break;
            default:
                break;
        }

        if (record->leaves() == 0) {
            close(*record);
            return Applied{true, true, symbol};
        }
        return Applied{true, false, symbol};
    }

    [[nodiscard]] std::int64_t position(SymbolId symbol) const noexcept {
        HOTPATH_HOT_ASSERT(symbol.raw() < symbols_);
        return positions_[symbol.raw()];
    }

    [[nodiscard]] std::int64_t cash_ticks(SymbolId symbol) const noexcept {
        HOTPATH_HOT_ASSERT(symbol.raw() < symbols_);
        return cash_[symbol.raw()];
    }

    [[nodiscard]] std::uint32_t open_orders() const noexcept { return open_; }
    [[nodiscard]] std::uint16_t symbols() const noexcept { return symbols_; }

private:
    void close(OrderRecord& record) noexcept {
        record.open = false;
        open_ -= 1U;
    }

    Applied replaced(const gateway::ExecutionReport& report) noexcept {
        OrderRecord* old_record = find(report.related_id);
        if (old_record == nullptr) {
            return Applied{};
        }
        OrderRecord moved = *old_record;
        close(*old_record);
        OrderRecord& slot = records_[report.client_id & mask_];
        if (slot.open) {
            return Applied{true, true, moved.symbol};
        }
        moved.id = report.client_id;
        moved.qty = report.qty + moved.filled;
        moved.canceled = 0;
        moved.price_ticks = report.price_ticks;
        slot = moved;
        open_ += 1U;
        return Applied{true, false, moved.symbol};
    }

    OrderRecord* records_{nullptr};
    std::int64_t* positions_{nullptr};
    std::int64_t* cash_{nullptr};
    std::uint32_t mask_{0};
    std::uint32_t open_{0};
    std::uint16_t symbols_{0};
};

} // namespace hotpath::strategy

#endif // HOTPATH_STRATEGY_ORDER_TABLE_HPP
