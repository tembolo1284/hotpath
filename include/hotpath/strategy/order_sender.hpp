#ifndef HOTPATH_STRATEGY_ORDER_SENDER_HPP
#define HOTPATH_STRATEGY_ORDER_SENDER_HPP

#include <cstdint>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/gateway/order_messages.hpp"

namespace hotpath::strategy {

template <gateway::RequestSink Sink, typename Clock = TscClock>
class OrderSender {
public:
    explicit OrderSender(Sink& sink, std::uint64_t first_id = 1) noexcept
        : sink_{&sink}, next_id_{first_id} {}

    void set_origin(std::uint64_t origin_tsc) noexcept { origin_tsc_ = origin_tsc; }

    [[nodiscard]] std::uint64_t next_id() const noexcept { return next_id_; }
    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

    [[nodiscard]] std::uint64_t send_new(SymbolId symbol, Side side, std::uint32_t qty,
                                         std::uint32_t price_ticks,
                                         std::uint32_t time_in_force) noexcept {
        gateway::OrderRequest* out = begin(gateway::RequestKind::New);
        if (out == nullptr) {
            return 0;
        }
        out->client_id = next_id_;
        out->qty = qty;
        out->price_ticks = price_ticks;
        out->time_in_force = time_in_force;
        out->symbol = symbol;
        out->side = side;
        return commit();
    }

    [[nodiscard]] bool cancel(std::uint64_t id, std::uint32_t intended_size = 0) noexcept {
        gateway::OrderRequest* out = begin(gateway::RequestKind::Cancel);
        if (out == nullptr) {
            return false;
        }
        out->client_id = id;
        out->qty = intended_size;
        sink_->publish();
        sent_ += 1;
        return true;
    }

    [[nodiscard]] std::uint64_t replace(std::uint64_t id, std::uint32_t qty,
                                        std::uint32_t price_ticks,
                                        std::uint32_t time_in_force) noexcept {
        gateway::OrderRequest* out = begin(gateway::RequestKind::Replace);
        if (out == nullptr) {
            return 0;
        }
        out->client_id = next_id_;
        out->target_id = id;
        out->qty = qty;
        out->price_ticks = price_ticks;
        out->time_in_force = time_in_force;
        return commit();
    }

private:
    [[nodiscard]] gateway::OrderRequest* begin(gateway::RequestKind kind) noexcept {
        gateway::OrderRequest* out = sink_->claim();
        if (out == nullptr) {
            refused_ += 1;
            return nullptr;
        }
        *out = gateway::OrderRequest{};
        out->kind = kind;
        out->origin_tsc = origin_tsc_;
        out->decision_tsc = Clock::now();
        return out;
    }

    [[nodiscard]] std::uint64_t commit() noexcept {
        const std::uint64_t id = next_id_;
        next_id_ += 1;
        sink_->publish();
        sent_ += 1;
        return id;
    }

    Sink* sink_;
    std::uint64_t next_id_;
    std::uint64_t origin_tsc_{0};
    std::uint64_t sent_{0};
    std::uint64_t refused_{0};
};

} // namespace hotpath::strategy

#endif // HOTPATH_STRATEGY_ORDER_SENDER_HPP
