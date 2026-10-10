#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <vector>

#include "hotpath/book/level_side.hpp"
#include "hotpath/core/types.hpp"
#include "support/bench.hpp"
#include "support/flow.hpp"

using namespace hotpath;

namespace {

struct Op {
    std::uint32_t ticks{0};
    std::uint32_t qty{0};
    bool add{false};
    bool gone{false};
};

struct Profile {
    const char* name;
    std::uint32_t width;
    std::uint32_t live;
};

std::vector<Op> make_ops(const Profile& profile, std::size_t count, std::uint64_t seed) {
    struct Resting {
        std::uint32_t ticks;
        std::uint32_t qty;
    };
    bench::Xorshift rng{seed};
    std::vector<Resting> live;
    std::vector<Op> ops;
    ops.reserve(count + profile.live + 64);
    std::uint32_t mid = 1'000'000;

    const auto take = [&](std::size_t index) {
        ops.push_back(Op{live[index].ticks, live[index].qty, false, true});
        live[index] = live.back();
        live.pop_back();
    };

    while (ops.size() < count) {
        const std::uint32_t roll = rng.below(100);
        if (roll < 2) {
            if (rng.below(2) == 0) {
                mid += 1;
            } else {
                mid -= 1;
                for (std::size_t i = live.size(); i > 0; --i) {
                    if (live[i - 1].ticks >= mid) {
                        take(i - 1);
                    }
                }
            }
        } else if (live.size() < profile.live && (roll < 60 || live.size() < profile.live / 2)) {
            const std::uint32_t a = rng.below(profile.width);
            const std::uint32_t b = rng.below(profile.width);
            const std::uint32_t offset = 1U + (a < b ? a : b);
            const std::uint32_t qty = 100U * (1U + rng.below(5));
            live.push_back(Resting{mid - offset, qty});
            ops.push_back(Op{mid - offset, qty, true, false});
        } else if (!live.empty()) {
            const std::size_t index = rng.below(static_cast<std::uint32_t>(live.size()));
            if (roll < 75 && live[index].qty > 100) {
                live[index].qty -= 100;
                ops.push_back(Op{live[index].ticks, 100, false, false});
            } else {
                take(index);
            }
        }
    }
    while (!live.empty()) {
        take(live.size() - 1);
    }
    return ops;
}

struct Best {
    std::uint32_t ticks{0};
    std::uint64_t qty{0};
    std::uint32_t depth{0};
    bool operator==(const Best&) const = default;
};

class ArrayLevels {
public:
    explicit ArrayLevels(mem::Arena& arena) {
        bench::require(side_.init(arena, 8192), "level side init");
    }
    void apply(const Op& op) noexcept {
        if (op.add) {
            side_.add(op.ticks, op.qty);
        } else {
            side_.reduce(op.ticks, op.qty, op.gone);
        }
    }
    [[nodiscard]] Best best() const noexcept {
        const book::Level level = side_.best();
        return Best{level.ticks, level.qty, side_.depth()};
    }

private:
    book::LevelSide<Side::Buy> side_{};
};

template <typename Map>
class TreeLevels {
public:
    explicit TreeLevels(Map&& map) : map_{std::move(map)} {}

    void apply(const Op& op) {
        if (op.add) {
            book::Level& level = map_.try_emplace(op.ticks).first->second;
            level.ticks = op.ticks;
            level.qty += op.qty;
            level.orders += 1;
            return;
        }
        const auto it = map_.find(op.ticks);
        it->second.qty -= op.qty;
        if (op.gone) {
            it->second.orders -= 1;
            if (it->second.orders == 0) {
                map_.erase(it);
            }
        }
    }
    [[nodiscard]] Best best() const {
        if (map_.empty()) {
            return Best{};
        }
        const book::Level& level = map_.begin()->second;
        return Best{level.ticks, level.qty, static_cast<std::uint32_t>(map_.size())};
    }

private:
    Map map_;
};

class LadderLevels {
public:
    static constexpr std::uint32_t kSlots = 1U << 16;

    explicit LadderLevels(std::uint32_t base) : base_{base}, slots_(kSlots) {}

    void apply(const Op& op) noexcept {
        const std::uint32_t index = op.ticks - base_;
        book::Level& level = slots_[index];
        if (op.add) {
            level.ticks = op.ticks;
            level.qty += op.qty;
            level.orders += 1;
            depth_ += level.orders == 1 ? 1U : 0U;
            if (depth_ == 1 || index > best_) {
                best_ = index;
            }
            return;
        }
        level.qty -= op.qty;
        if (op.gone) {
            level.orders -= 1;
            if (level.orders == 0) {
                depth_ -= 1;
                if (index == best_ && depth_ != 0) {
                    while (slots_[best_].orders == 0) {
                        best_ -= 1;
                    }
                }
            }
        }
    }
    [[nodiscard]] Best best() const noexcept {
        if (depth_ == 0) {
            return Best{};
        }
        return Best{slots_[best_].ticks, slots_[best_].qty, depth_};
    }

private:
    std::uint32_t base_;
    std::uint32_t best_{0};
    std::uint32_t depth_{0};
    std::vector<book::Level> slots_;
};

template <typename Levels>
Best replay_once(Levels& levels, const std::vector<Op>& ops, std::size_t upto) {
    for (std::size_t i = 0; i < upto; ++i) {
        levels.apply(ops[i]);
    }
    const Best seen = levels.best();
    for (std::size_t i = upto; i < ops.size(); ++i) {
        levels.apply(ops[i]);
    }
    return seen;
}

template <typename Levels>
void measure(bench::Session& session, const char* name, Levels& levels,
             const std::vector<Op>& ops) {
    session.run(name, ops.size(), [] {}, [&](std::uint64_t i) {
        levels.apply(ops[i]);
        bench::keep(levels.best().qty);
    });
}

using StdMap = std::map<std::uint32_t, book::Level, std::greater<>>;
using PoolMap = std::pmr::map<std::uint32_t, book::Level, std::greater<>>;

void run_profile(bench::Session& session, const Profile& profile) {
    const std::vector<Op> ops = make_ops(profile, session.ops(2'000'000), 0xB00C'0000ULL + profile.width);

    bench::Workspace workspace{std::size_t{1} << 20};
    ArrayLevels array{workspace.arena()};
    TreeLevels<StdMap> tree{StdMap{}};
    std::vector<std::byte> pool_bytes(std::size_t{8} << 20);
    std::pmr::monotonic_buffer_resource upstream{pool_bytes.data(), pool_bytes.size(),
                                                 std::pmr::null_memory_resource()};
    std::pmr::unsynchronized_pool_resource pool{&upstream};
    TreeLevels<PoolMap> pooled{PoolMap{&pool}};
    LadderLevels ladder{1'000'000U - LadderLevels::kSlots / 2U};

    const std::size_t probe = ops.size() / 2;
    const Best expected = replay_once(array, ops, probe);
    if (replay_once(tree, ops, probe) != expected || replay_once(pooled, ops, probe) != expected ||
        replay_once(ladder, ops, probe) != expected || array.best().depth != 0) {
        bench::die("level structures disagree");
    }

    std::printf("-- %s: orders within %u ticks of the touch, about %u resting, depth %u at the "
                "midpoint --\n",
                profile.name, profile.width, profile.live, expected.depth);
    char label[48];
    std::snprintf(label, sizeof(label), "%s array", profile.name);
    measure(session, label, array, ops);
    std::snprintf(label, sizeof(label), "%s map", profile.name);
    measure(session, label, tree, ops);
    std::snprintf(label, sizeof(label), "%s map+pool", profile.name);
    measure(session, label, pooled, ops);
    std::snprintf(label, sizeof(label), "%s ladder", profile.name);
    measure(session, label, ladder, ops);
}

} // namespace

int main(int argc, char** argv) {
    bench::Session session{argc, argv,
                           "levels_bench: sorted array vs red-black tree vs price ladder"};
    session.note("each op is one add or reduce followed by a best-level read");
    for (const Profile& profile : {Profile{"tight", 16, 200}, Profile{"medium", 200, 2'000},
                                   Profile{"deep", 4'000, 40'000}}) {
        run_profile(session, profile);
    }
    return 0;
}
