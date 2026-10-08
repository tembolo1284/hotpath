#ifndef HOTPATH_IPC_SHM_HPP
#define HOTPATH_IPC_SHM_HPP

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <limits>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"

namespace hotpath::ipc {

inline constexpr int kMadvPopulateWrite = 23;
inline constexpr std::size_t kShmPageBytes = 4096;

class ShmSegment {
public:
    ShmSegment() noexcept = default;

    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;

    ShmSegment(ShmSegment&& other) noexcept : addr_{other.addr_}, len_{other.len_} {
        other.addr_ = nullptr;
        other.len_ = 0;
    }

    ShmSegment& operator=(ShmSegment&& other) noexcept {
        if (this != &other) {
            release();
            addr_ = other.addr_;
            len_ = other.len_;
            other.addr_ = nullptr;
            other.len_ = 0;
        }
        return *this;
    }

    ~ShmSegment() { release(); }

    [[nodiscard]] Status create(const char* name, std::size_t len) noexcept {
        HOTPATH_ASSERT(addr_ == nullptr);
        constexpr auto kMaxLen = static_cast<std::size_t>(std::numeric_limits<off_t>::max());
        if (name == nullptr || len == 0 || len > kMaxLen - (kShmPageBytes - 1)) {
            return Status{EINVAL};
        }
        const std::size_t rounded = (len + kShmPageBytes - 1) & ~(kShmPageBytes - 1);

        const int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            return Status::from_errno();
        }

        Status st{};
        if (::ftruncate(fd, static_cast<off_t>(rounded)) != 0) {
            st = Status::from_errno();
        } else {
            st = map_fd(fd, rounded);
        }
        static_cast<void>(::close(fd));

        if (!st.ok()) {
            static_cast<void>(::shm_unlink(name));
        }
        return st;
    }

    [[nodiscard]] Status open(const char* name) noexcept {
        HOTPATH_ASSERT(addr_ == nullptr);
        if (name == nullptr) {
            return Status{EINVAL};
        }

        const int fd = ::shm_open(name, O_RDWR, 0);
        if (fd < 0) {
            return Status::from_errno();
        }

        Status st{};
        struct stat info {};
        if (::fstat(fd, &info) != 0) {
            st = Status::from_errno();
        } else if (info.st_size <= 0) {
            st = Status{EAGAIN};
        } else {
            st = map_fd(fd, static_cast<std::size_t>(info.st_size));
        }
        static_cast<void>(::close(fd));
        return st;
    }

    [[nodiscard]] Status populate() noexcept {
        HOTPATH_ASSERT(addr_ != nullptr);
        if (::madvise(addr_, len_, kMadvPopulateWrite) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    void reset() noexcept { release(); }

    [[nodiscard]] static Status unlink(const char* name) noexcept {
        if (name == nullptr) {
            return Status{EINVAL};
        }
        if (::shm_unlink(name) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    [[nodiscard]] void* data() const noexcept { return addr_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] bool mapped() const noexcept { return addr_ != nullptr; }

private:
    [[nodiscard]] Status map_fd(int fd, std::size_t len) noexcept {
        void* addr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            return Status::from_errno();
        }
        addr_ = addr;
        len_ = len;
        return Status{};
    }

    void release() noexcept {
        if (addr_ != nullptr) {
            const int rc = ::munmap(addr_, len_);
            HOTPATH_ASSERT(rc == 0);
            addr_ = nullptr;
            len_ = 0;
        }
    }

    void* addr_{nullptr};
    std::size_t len_{0};
};

} // namespace hotpath::ipc

#endif // HOTPATH_IPC_SHM_HPP
