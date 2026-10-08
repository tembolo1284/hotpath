#ifndef HOTPATH_IPC_CHANNEL_HPP
#define HOTPATH_IPC_CHANNEL_HPP

#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/ipc/shm.hpp"
#include "hotpath/ipc/spsc_ring.hpp"

namespace hotpath::ipc {

template <ShmSafe T>
class Channel {
public:
    Channel() noexcept = default;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] Status create(const char* name, std::uint64_t capacity) noexcept {
        HOTPATH_ASSERT(ring_ == nullptr);
        if (!SpscRing<T>::valid_capacity(capacity)) {
            return Status{EINVAL};
        }

        Status st = segment_.create(name, SpscRing<T>::bytes_for(capacity));
        if (!st.ok()) {
            return st;
        }

        st = segment_.populate();
        if (st.ok()) {
            const auto made = SpscRing<T>::format(segment_.data(), segment_.size(), capacity);
            st = made.status;
            ring_ = made.ring;
        }
        if (!st.ok()) {
            segment_.reset();
            static_cast<void>(ShmSegment::unlink(name));
        }
        return st;
    }

    [[nodiscard]] Status open(const char* name) noexcept {
        HOTPATH_ASSERT(ring_ == nullptr);

        Status st = segment_.open(name);
        if (!st.ok()) {
            return st;
        }

        const auto found = SpscRing<T>::attach(segment_.data(), segment_.size());
        st = found.status;
        if (st.ok()) {
            st = segment_.populate();
        }
        if (!st.ok()) {
            segment_.reset();
            return st;
        }
        ring_ = found.ring;
        return Status{};
    }

    [[nodiscard]] static Status unlink(const char* name) noexcept {
        return ShmSegment::unlink(name);
    }

    [[nodiscard]] SpscProducer<T> producer() noexcept {
        HOTPATH_ASSERT(ring_ != nullptr);
        return SpscProducer<T>{*ring_};
    }

    [[nodiscard]] SpscConsumer<T> consumer() noexcept {
        HOTPATH_ASSERT(ring_ != nullptr);
        return SpscConsumer<T>{*ring_};
    }

    [[nodiscard]] bool ready() const noexcept { return ring_ != nullptr; }
    [[nodiscard]] std::uint64_t capacity() const noexcept {
        HOTPATH_ASSERT(ring_ != nullptr);
        return ring_->capacity();
    }

private:
    ShmSegment segment_{};
    SpscRing<T>* ring_{nullptr};
};

} // namespace hotpath::ipc

#endif // HOTPATH_IPC_CHANNEL_HPP
