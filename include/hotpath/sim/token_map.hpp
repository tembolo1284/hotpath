#ifndef HOTPATH_SIM_TOKEN_MAP_HPP
#define HOTPATH_SIM_TOKEN_MAP_HPP

#include <bit>
#include <cstdint>
#include <string_view>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/sim/matching_engine.hpp"
#include "hotpath/wire/ouch.hpp"

namespace hotpath::sim {

struct TokenEntry {
    wire::ouch::Token token{};
    OrderHandle handle{};
    std::uint16_t symbol{0};
    Side side{Side::Buy};
    bool used{false};
    bool live{false};
};

class TokenMap {
public:
    [[nodiscard]] Status init(mem::Arena& arena, std::uint32_t capacity) noexcept {
        HOTPATH_ASSERT(entries_ == nullptr);
        if (capacity < 16 || !std::has_single_bit(capacity)) {
            return Status{EINVAL};
        }
        TokenEntry* entries = arena.allocate_array<TokenEntry>(capacity);
        if (entries == nullptr) {
            return Status{ENOMEM};
        }
        entries_ = entries;
        mask_ = capacity - 1U;
        max_load_ = capacity - capacity / 4U;
        return Status{};
    }

    [[nodiscard]] TokenEntry* find(const wire::ouch::Token& token) noexcept {
        std::uint32_t index = hash(token) & mask_;
        for (std::uint32_t probe = 0; probe <= mask_; ++probe) {
            TokenEntry& entry = entries_[index];
            if (!entry.used) {
                return nullptr;
            }
            if (entry.token == token) {
                return &entry;
            }
            index = (index + 1U) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] TokenEntry* insert(const wire::ouch::Token& token) noexcept {
        if (size_ >= max_load_) {
            return nullptr;
        }
        std::uint32_t index = hash(token) & mask_;
        for (std::uint32_t probe = 0; probe <= mask_; ++probe) {
            TokenEntry& entry = entries_[index];
            if (!entry.used) {
                entry = TokenEntry{};
                entry.token = token;
                entry.used = true;
                size_ += 1U;
                return &entry;
            }
            if (entry.token == token) {
                return nullptr;
            }
            index = (index + 1U) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] std::uint64_t user_of(const TokenEntry& entry) const noexcept {
        return static_cast<std::uint64_t>(&entry - entries_) + 1U;
    }

    [[nodiscard]] TokenEntry* by_user(std::uint64_t user) noexcept {
        if (user == 0 || user > std::uint64_t{mask_} + 1U) {
            return nullptr;
        }
        TokenEntry& entry = entries_[user - 1U];
        return entry.used ? &entry : nullptr;
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

private:
    [[nodiscard]] static std::uint32_t hash(const wire::ouch::Token& token) noexcept {
        std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
        for (const char c : token.raw()) {
            h = (h ^ static_cast<unsigned char>(c)) * 0x0000'0100'0000'01B3ULL;
        }
        return static_cast<std::uint32_t>(h ^ (h >> 32));
    }

    TokenEntry* entries_{nullptr};
    std::uint32_t mask_{0};
    std::uint32_t size_{0};
    std::uint32_t max_load_{0};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_TOKEN_MAP_HPP
