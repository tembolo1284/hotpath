#ifndef HOTPATH_WIRE_FIELDS_HPP
#define HOTPATH_WIRE_FIELDS_HPP

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace hotpath::wire {

static_assert(std::endian::native == std::endian::little);

enum class Parse : std::uint8_t { Ok, Unknown, Truncated, Malformed };

template <std::unsigned_integral T, std::size_t N = sizeof(T)>
    requires(N >= 1 && N <= sizeof(T))
class BigEndian {
public:
    constexpr BigEndian() noexcept = default;
    constexpr explicit BigEndian(T value) noexcept { set(value); }

    [[nodiscard]] constexpr T get() const noexcept {
        if (std::is_constant_evaluated()) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < N; ++i) {
                value = (value << 8) | bytes_[i];
            }
            return static_cast<T>(value);
        }
        std::uint64_t raw = 0;
        std::memcpy(&raw, bytes_, N);
        return static_cast<T>(__builtin_bswap64(raw) >> kShift);
    }

    constexpr void set(T value) noexcept {
        const std::uint64_t wide = value;
        if (std::is_constant_evaluated()) {
            for (std::size_t i = 0; i < N; ++i) {
                bytes_[N - 1 - i] = static_cast<std::uint8_t>(wide >> (8 * i));
            }
            return;
        }
        const std::uint64_t raw = __builtin_bswap64(wide << kShift);
        std::memcpy(bytes_, &raw, N);
    }

    constexpr bool operator==(const BigEndian&) const noexcept = default;

private:
    static constexpr unsigned kShift = 8U * static_cast<unsigned>(sizeof(std::uint64_t) - N);

    std::uint8_t bytes_[N]{};
};

using U16 = BigEndian<std::uint16_t>;
using U32 = BigEndian<std::uint32_t>;
using U48 = BigEndian<std::uint64_t, 6>;
using U64 = BigEndian<std::uint64_t>;

template <std::size_t N>
    requires(N >= 1)
class Alpha {
public:
    constexpr Alpha() noexcept { fill(std::string_view{}); }
    constexpr explicit Alpha(std::string_view text) noexcept { fill(text); }

    [[nodiscard]] static constexpr Alpha right_justified(std::string_view text) noexcept {
        Alpha out;
        const std::size_t len = text.size() < N ? text.size() : N;
        for (std::size_t i = 0; i < len; ++i) {
            out.chars_[N - len + i] = text[i];
        }
        return out;
    }

    [[nodiscard]] constexpr std::string_view raw() const noexcept {
        return std::string_view{chars_, N};
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        std::size_t len = N;
        for (std::size_t i = 0; i < N && chars_[len - 1] == ' '; ++i) {
            len -= 1;
            if (len == 0) {
                break;
            }
        }
        return std::string_view{chars_, len};
    }

    [[nodiscard]] constexpr std::string_view trimmed() const noexcept {
        std::string_view out = view();
        for (std::size_t i = 0; i < N && !out.empty() && out.front() == ' '; ++i) {
            out.remove_prefix(1);
        }
        return out;
    }

    constexpr bool operator==(const Alpha&) const noexcept = default;

private:
    constexpr void fill(std::string_view text) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            chars_[i] = i < text.size() ? text[i] : ' ';
        }
    }

    char chars_[N];
};

struct Numeric {
    std::uint64_t value{0};
    bool ok{false};
};

[[nodiscard]] constexpr Numeric parse_numeric(std::string_view text) noexcept {
    constexpr std::size_t kMaxDigits = 19;
    std::uint64_t value = 0;
    std::size_t digits = 0;
    bool seen_digit = false;
    bool trailing = false;

    for (const char c : text) {
        if (c == ' ') {
            trailing = seen_digit;
        } else if (c >= '0' && c <= '9' && !trailing && digits < kMaxDigits) {
            value = value * 10U + static_cast<std::uint64_t>(c - '0');
            digits += 1;
            seen_digit = true;
        } else {
            return Numeric{0, false};
        }
    }
    return Numeric{value, seen_digit};
}

template <std::size_t N>
[[nodiscard]] constexpr Alpha<N> format_numeric(std::uint64_t value) noexcept {
    char digits[N];
    for (std::size_t i = 0; i < N; ++i) {
        digits[i] = ' ';
    }
    std::uint64_t rest = value;
    for (std::size_t i = 0; i < N; ++i) {
        digits[N - 1 - i] = static_cast<char>('0' + static_cast<char>(rest % 10U));
        rest /= 10U;
        if (rest == 0) {
            break;
        }
    }
    return Alpha<N>{std::string_view{digits, N}};
}

template <typename T>
concept WireMessage = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                      alignof(T) == 1;

static_assert(sizeof(U16) == 2 && sizeof(U32) == 4 && sizeof(U48) == 6 && sizeof(U64) == 8);
static_assert(alignof(U64) == 1 && alignof(Alpha<8>) == 1);
static_assert(U16{0x1234}.get() == 0x1234);
static_assert(U48{0x0102'0304'0506ULL}.get() == 0x0102'0304'0506ULL);
static_assert(U64{0x0102'0304'0506'0708ULL}.get() == 0x0102'0304'0506'0708ULL);
static_assert(Alpha<8>{"AAPL"}.raw() == "AAPL    ");
static_assert(Alpha<8>{"AAPL"}.view() == "AAPL");
static_assert(Alpha<4>{}.view().empty());
static_assert(Alpha<10>::right_justified("S1").raw() == "        S1");
static_assert(Alpha<10>::right_justified("S1").trimmed() == "S1");
static_assert(Alpha<4>{}.trimmed().empty());
static_assert(parse_numeric("                  42").value == 42);
static_assert(parse_numeric("7   ").ok && !parse_numeric("    ").ok && !parse_numeric("4 2").ok);
static_assert(format_numeric<20>(42).raw() == "                  42");
static_assert(format_numeric<3>(0).raw() == "  0");

} // namespace hotpath::wire

#endif // HOTPATH_WIRE_FIELDS_HPP
