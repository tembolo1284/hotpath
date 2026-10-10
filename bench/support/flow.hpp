#ifndef HOTPATH_BENCH_SUPPORT_FLOW_HPP
#define HOTPATH_BENCH_SUPPORT_FLOW_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/net/socket.hpp"
#include "hotpath/sim/itch_publisher.hpp"
#include "hotpath/sim/liquidity.hpp"
#include "hotpath/sim/matching_engine.hpp"
#include "hotpath/sim/token_map.hpp"
#include "hotpath/sim/venue.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/mold.hpp"
#include "support/bench.hpp"

namespace hotpath::bench {

inline constexpr std::uint16_t kFlowSymbols = 4;
inline constexpr std::array<wire::Alpha<8>, kFlowSymbols> kFlowTickers{
    wire::Alpha<8>{"MSFT"}, wire::Alpha<8>{"NVDA"}, wire::Alpha<8>{"AAPL"},
    wire::Alpha<8>{"AMZN"}};

class Workspace {
public:
    explicit Workspace(std::size_t bytes) noexcept {
        require(region_.map(bytes, sys::PageKind::Small), "cannot map memory");
        region_.prefault();
        arena_ = mem::Arena{region_};
    }

    [[nodiscard]] mem::Arena& arena() noexcept { return arena_; }

private:
    mem::Region region_{};
    mem::Arena arena_{};
};

class PacketLog {
public:
    net::IoResult send(std::span<const std::byte> packet) {
        offsets_.push_back(bytes_.size());
        bytes_.insert(bytes_.end(), packet.begin(), packet.end());
        return net::IoResult{packet.size(), Status{}, false};
    }

    [[nodiscard]] std::size_t size() const noexcept { return offsets_.size(); }

    [[nodiscard]] std::span<const std::byte> operator[](std::size_t index) const noexcept {
        const std::size_t begin = offsets_[index];
        const std::size_t end = index + 1 < offsets_.size() ? offsets_[index + 1] : bytes_.size();
        return std::span<const std::byte>{bytes_.data() + begin, end - begin};
    }

private:
    std::vector<std::byte> bytes_;
    std::vector<std::size_t> offsets_;
};

struct NullSession {
    template <wire::WireMessage M>
    void send_sequenced(const M&) noexcept {}
};

struct Flow {
    PacketLog packets;
    std::uint64_t messages{0};
};

inline void generate_flow(Flow& flow, std::uint64_t steps, std::uint64_t seed) {
    using Publisher = sim::ItchPublisher<PacketLog>;

    Workspace workspace{std::size_t{128} << 20};
    mem::Arena& arena = workspace.arena();

    sim::MatchingEngine engine;
    sim::TokenMap tokens;
    NullSession session;
    Publisher publisher{flow.packets};
    sim::Venue<Publisher, NullSession> venue{engine, tokens, publisher, session, kFlowTickers};
    sim::Liquidity liquidity;

    require(engine.init(arena, {.symbols = kFlowSymbols, .max_orders = 1U << 14,
                                .levels_per_side = 256}),
            "engine init");
    require(tokens.init(arena, 1U << 10), "token map init");
    require(publisher.init(arena, {.store_capacity = 1U << 12}), "publisher init");
    require(liquidity.init(arena, kFlowSymbols, {.seed = seed, .max_live = 48}), "liquidity init");

    std::uint64_t now_ns = 34'200'000'000'000ULL;
    venue.set_time(now_ns);
    venue.open_market();
    publisher.flush();
    for (std::uint64_t i = 0; i < steps; ++i) {
        now_ns += 250'000;
        venue.set_time(now_ns);
        liquidity.step(venue);
        publisher.flush();
    }
    flow.messages = publisher.next_sequence() - 1;
}

} // namespace hotpath::bench

#endif // HOTPATH_BENCH_SUPPORT_FLOW_HPP
