#ifndef HOTPATH_FEED_SEQUENCER_HPP
#define HOTPATH_FEED_SEQUENCER_HPP

#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/mold.hpp"

namespace hotpath::feed {

enum class Verdict : std::uint8_t {
    Process,
    Duplicate,
    Gap,
    Heartbeat,
    EndOfSession,
    WrongSession,
};

struct Decision {
    Verdict verdict{Verdict::Process};
    std::uint64_t missing{0};
};

class Sequencer {
public:
    constexpr Sequencer() noexcept = default;
    explicit constexpr Sequencer(std::uint64_t start_sequence) noexcept
        : expected_{start_sequence} {}

    [[nodiscard]] Decision inspect(const wire::Alpha<10>& session, std::uint64_t sequence,
                                   std::uint16_t count) noexcept {
        if (!has_session_) {
            session_ = session;
            has_session_ = true;
            if (expected_ == 0) {
                expected_ = sequence;
            }
        } else if (session != session_) {
            return Decision{Verdict::WrongSession, 0};
        }

        if (sequence > expected_) {
            return Decision{Verdict::Gap, sequence - expected_};
        }
        if (count == wire::mold::kEndOfSession) {
            return Decision{Verdict::EndOfSession, 0};
        }
        if (count == wire::mold::kHeartbeat) {
            return Decision{Verdict::Heartbeat, 0};
        }
        if (sequence + count <= expected_) {
            return Decision{Verdict::Duplicate, 0};
        }
        return Decision{Verdict::Process, 0};
    }

    void commit(std::uint64_t next_sequence) noexcept {
        HOTPATH_HOT_ASSERT(next_sequence >= expected_);
        expected_ = next_sequence;
    }

    [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
    [[nodiscard]] bool has_session() const noexcept { return has_session_; }
    [[nodiscard]] const wire::Alpha<10>& session() const noexcept { return session_; }

private:
    wire::Alpha<10> session_{};
    std::uint64_t expected_{1};
    bool has_session_{false};
};

} // namespace hotpath::feed

#endif // HOTPATH_FEED_SEQUENCER_HPP
