#ifndef HOTPATH_FEED_SYMBOL_TABLE_HPP
#define HOTPATH_FEED_SYMBOL_TABLE_HPP

#include <cstdint>
#include <string_view>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/wire/fields.hpp"

namespace hotpath::feed {

class SymbolTable {
public:
    static constexpr std::uint16_t kNone = 0xFFFF;
    static constexpr std::uint32_t kLocates = 65'536;

    [[nodiscard]] Status init(mem::Arena& arena, std::uint16_t max_symbols) noexcept {
        HOTPATH_ASSERT(by_locate_ == nullptr);
        if (max_symbols == 0 || max_symbols == kNone) {
            return Status{EINVAL};
        }
        std::uint16_t* by_locate = arena.allocate_array<std::uint16_t>(kLocates);
        wire::Alpha<8>* tickers = arena.allocate_array<wire::Alpha<8>>(max_symbols);
        std::uint16_t* locates = arena.allocate_array<std::uint16_t>(max_symbols);
        char* states = arena.allocate_array<char>(max_symbols);
        if (by_locate == nullptr || tickers == nullptr || locates == nullptr || states == nullptr) {
            return Status{ENOMEM};
        }
        for (std::uint32_t i = 0; i < kLocates; ++i) {
            by_locate[i] = kNone;
        }
        for (std::uint16_t i = 0; i < max_symbols; ++i) {
            locates[i] = kNone;
            states[i] = 'T';
        }
        by_locate_ = by_locate;
        tickers_ = tickers;
        locates_ = locates;
        states_ = states;
        capacity_ = max_symbols;
        return Status{};
    }

    [[nodiscard]] Status watch(std::string_view ticker) noexcept {
        HOTPATH_ASSERT(by_locate_ != nullptr);
        if (ticker.empty() || ticker.size() > 8) {
            return Status{EINVAL};
        }
        const wire::Alpha<8> key{ticker};
        if (find(key) != kNone) {
            return Status{EEXIST};
        }
        if (count_ == capacity_) {
            return Status{ENOSPC};
        }
        tickers_[count_] = key;
        count_ = static_cast<std::uint16_t>(count_ + 1U);
        return Status{};
    }

    std::uint16_t resolve(std::uint16_t locate, const wire::Alpha<8>& stock) noexcept {
        const std::uint16_t index = find(stock);
        if (index == kNone) {
            return kNone;
        }
        if (locates_[index] != kNone) {
            by_locate_[locates_[index]] = kNone;
        } else {
            resolved_ = static_cast<std::uint16_t>(resolved_ + 1U);
        }
        locates_[index] = locate;
        by_locate_[locate] = index;
        return index;
    }

    [[nodiscard]] [[gnu::always_inline]] std::uint16_t lookup(std::uint16_t locate) const noexcept {
        return by_locate_[locate];
    }

    [[nodiscard]] std::uint16_t find(const wire::Alpha<8>& ticker) const noexcept {
        for (std::uint16_t i = 0; i < count_; ++i) {
            if (tickers_[i] == ticker) {
                return i;
            }
        }
        return kNone;
    }

    [[nodiscard]] const wire::Alpha<8>& ticker(SymbolId symbol) const noexcept {
        HOTPATH_ASSERT(symbol.raw() < count_);
        return tickers_[symbol.raw()];
    }

    [[nodiscard]] char trading_state(SymbolId symbol) const noexcept {
        HOTPATH_HOT_ASSERT(symbol.raw() < count_);
        return states_[symbol.raw()];
    }

    void set_trading_state(SymbolId symbol, char state) noexcept {
        HOTPATH_HOT_ASSERT(symbol.raw() < count_);
        states_[symbol.raw()] = state;
    }

    [[nodiscard]] bool halted(SymbolId symbol) const noexcept {
        return trading_state(symbol) != 'T';
    }

    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint16_t resolved() const noexcept { return resolved_; }
    [[nodiscard]] std::uint16_t capacity() const noexcept { return capacity_; }

private:
    std::uint16_t* by_locate_{nullptr};
    wire::Alpha<8>* tickers_{nullptr};
    std::uint16_t* locates_{nullptr};
    char* states_{nullptr};
    std::uint16_t capacity_{0};
    std::uint16_t count_{0};
    std::uint16_t resolved_{0};
};

} // namespace hotpath::feed

#endif // HOTPATH_FEED_SYMBOL_TABLE_HPP
