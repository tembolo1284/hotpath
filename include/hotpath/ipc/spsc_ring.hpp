#ifndef HOTPATH_IPC_SPSC_RING_HPP
#define HOTPATH_IPC_SPSC_RING_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"

namespace hotpath::ipc {

inline constexpr std::uint64_t kRingMagic = 0x484F'5450'5350'5343ULL;
inline constexpr std::uint32_t kRingVersion = 1;
inline constexpr std::uint64_t kRingMaxCapacity = 1ULL << 32;

template <ShmSafe T>
class SpscProducer;

template <ShmSafe T>
class SpscConsumer;

template <ShmSafe T>
class SpscRing {
public:
    struct Result {
        SpscRing* ring{nullptr};
        Status status{};
    };

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] static constexpr bool valid_capacity(std::uint64_t capacity) noexcept {
        return std::has_single_bit(capacity) && capacity <= kRingMaxCapacity;
    }

    [[nodiscard]] static constexpr std::size_t bytes_for(std::uint64_t capacity) noexcept {
        return sizeof(SpscRing) + static_cast<std::size_t>(capacity) * sizeof(T);
    }

    [[nodiscard]] static Result format(void* mem, std::size_t bytes,
                                       std::uint64_t capacity) noexcept {
        static_assert(sizeof(SpscRing) == 3 * kCacheLine);
        static_assert(alignof(SpscRing) == kCacheLine);
        static_assert(alignof(T) <= kCacheLine);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

        if (mem == nullptr || !valid_capacity(capacity) || !aligned(mem)) {
            return Result{nullptr, Status{EINVAL}};
        }
        if (bytes < bytes_for(capacity)) {
            return Result{nullptr, Status{ENOMEM}};
        }

        SpscRing* ring = new (mem) SpscRing{capacity};
        ring->magic_.store(kRingMagic, std::memory_order_release);
        return Result{ring, Status{}};
    }

    [[nodiscard]] static Result attach(void* mem, std::size_t bytes) noexcept {
        if (mem == nullptr || bytes < sizeof(SpscRing) || !aligned(mem)) {
            return Result{nullptr, Status{EINVAL}};
        }

        SpscRing* ring = std::launder(reinterpret_cast<SpscRing*>(mem));
        if (ring->magic_.load(std::memory_order_acquire) != kRingMagic) {
            return Result{nullptr, Status{EAGAIN}};
        }
        if (ring->version_ != kRingVersion || ring->elem_size_ != sizeof(T) ||
            ring->elem_align_ != alignof(T) || !valid_capacity(ring->capacity_)) {
            return Result{nullptr, Status{EPROTO}};
        }
        if (bytes < bytes_for(ring->capacity_)) {
            return Result{nullptr, Status{EPROTO}};
        }
        return Result{ring, Status{}};
    }

    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::uint64_t size_approx() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : 0;
    }

private:
    friend class SpscProducer<T>;
    friend class SpscConsumer<T>;

    explicit SpscRing(std::uint64_t capacity) noexcept : capacity_{capacity} {}

    [[nodiscard]] static bool aligned(const void* mem) noexcept {
        return (reinterpret_cast<std::uintptr_t>(mem) & (kCacheLine - 1)) == 0;
    }

    [[nodiscard]] T* slots() noexcept {
        return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + sizeof(SpscRing));
    }

    std::atomic<std::uint64_t> magic_{0};
    std::uint64_t capacity_;
    std::uint32_t version_{kRingVersion};
    std::uint32_t elem_size_{static_cast<std::uint32_t>(sizeof(T))};
    std::uint32_t elem_align_{static_cast<std::uint32_t>(alignof(T))};
    std::uint32_t reserved_{0};

    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
};

template <ShmSafe T>
class SpscProducer {
public:
    explicit SpscProducer(SpscRing<T>& ring) noexcept
        : ring_{&ring},
          slots_{ring.slots()},
          mask_{ring.capacity() - 1},
          tail_{ring.tail_.load(std::memory_order_relaxed)},
          cached_head_{ring.head_.load(std::memory_order_acquire)} {}

    SpscProducer(const SpscProducer&) = delete;
    SpscProducer& operator=(const SpscProducer&) = delete;

    [[nodiscard]] T* claim() noexcept {
        if (tail_ - cached_head_ > mask_) {
            cached_head_ = ring_->head_.load(std::memory_order_acquire);
            if (tail_ - cached_head_ > mask_) {
                return nullptr;
            }
        }
        return slots_ + (tail_ & mask_);
    }

    void publish() noexcept {
        HOTPATH_HOT_ASSERT(tail_ - cached_head_ <= mask_);
        tail_ += 1;
        ring_->tail_.store(tail_, std::memory_order_release);
    }

    [[nodiscard]] bool try_push(const T& value) noexcept {
        T* slot = claim();
        if (slot == nullptr) {
            return false;
        }
        *slot = value;
        publish();
        return true;
    }

private:
    SpscRing<T>* ring_;
    T* slots_;
    std::uint64_t mask_;
    std::uint64_t tail_;
    std::uint64_t cached_head_;
};

template <ShmSafe T>
class SpscConsumer {
public:
    explicit SpscConsumer(SpscRing<T>& ring) noexcept
        : ring_{&ring},
          slots_{ring.slots()},
          mask_{ring.capacity() - 1},
          head_{ring.head_.load(std::memory_order_relaxed)},
          cached_tail_{ring.tail_.load(std::memory_order_acquire)} {}

    SpscConsumer(const SpscConsumer&) = delete;
    SpscConsumer& operator=(const SpscConsumer&) = delete;

    [[nodiscard]] const T* peek() noexcept {
        if (head_ == cached_tail_) {
            cached_tail_ = ring_->tail_.load(std::memory_order_acquire);
            if (head_ == cached_tail_) {
                return nullptr;
            }
        }
        return slots_ + (head_ & mask_);
    }

    void consume() noexcept {
        HOTPATH_HOT_ASSERT(head_ != cached_tail_);
        head_ += 1;
        ring_->head_.store(head_, std::memory_order_release);
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const T* slot = peek();
        if (slot == nullptr) {
            return false;
        }
        out = *slot;
        consume();
        return true;
    }

private:
    SpscRing<T>* ring_;
    const T* slots_;
    std::uint64_t mask_;
    std::uint64_t head_;
    std::uint64_t cached_tail_;
};

} // namespace hotpath::ipc

#endif // HOTPATH_IPC_SPSC_RING_HPP
