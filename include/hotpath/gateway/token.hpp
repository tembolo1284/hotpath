#ifndef HOTPATH_GATEWAY_TOKEN_HPP
#define HOTPATH_GATEWAY_TOKEN_HPP

#include <array>
#include <cstdint>
#include <string_view>

#include "hotpath/wire/ouch.hpp"

namespace hotpath::gateway {

inline constexpr std::string_view kTokenAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
inline constexpr unsigned kTokenDigits = 13;

namespace detail {

[[nodiscard]] constexpr std::array<std::int8_t, 256> make_token_decode() noexcept {
    std::array<std::int8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = -1;
    }
    for (std::size_t i = 0; i < kTokenAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kTokenAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

inline constexpr std::array<std::int8_t, 256> kTokenDecode = make_token_decode();

} // namespace detail

[[nodiscard]] constexpr wire::ouch::Token make_token(char prefix, std::uint64_t id) noexcept {
    char text[1 + kTokenDigits]{};
    text[0] = prefix;
    for (unsigned i = 0; i < kTokenDigits; ++i) {
        text[kTokenDigits - i] = kTokenAlphabet[(id >> (5U * i)) & 31U];
    }
    return wire::ouch::Token{std::string_view{text, sizeof(text)}};
}

struct TokenId {
    std::uint64_t id{0};
    bool ok{false};
};

[[nodiscard]] constexpr TokenId parse_token(const wire::ouch::Token& token, char prefix) noexcept {
    const std::string_view text = token.raw();
    if (text[0] != prefix) {
        return TokenId{};
    }
    std::uint64_t id = 0;
    for (unsigned i = 1; i <= kTokenDigits; ++i) {
        const std::int8_t digit = detail::kTokenDecode[static_cast<unsigned char>(text[i])];
        if (digit < 0 || (i == 1 && digit > 15)) {
            return TokenId{};
        }
        id = (id << 5U) | static_cast<std::uint64_t>(digit);
    }
    return TokenId{id, true};
}

static_assert(make_token('H', 0).raw() == "H0000000000000");
static_assert(make_token('H', 33).raw() == "H0000000000011");
static_assert(parse_token(make_token('H', 0xFFFF'FFFF'FFFF'FFFFULL), 'H').id == 0xFFFF'FFFF'FFFF'FFFFULL);
static_assert(parse_token(make_token('H', 123'456'789), 'H').ok);
static_assert(!parse_token(make_token('H', 1), 'X').ok);
static_assert(!parse_token(wire::ouch::Token{"Hlowercase0000"}, 'H').ok);

} // namespace hotpath::gateway

#endif // HOTPATH_GATEWAY_TOKEN_HPP
