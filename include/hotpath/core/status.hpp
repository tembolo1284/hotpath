#ifndef HOTPATH_CORE_STATUS_HPP
#define HOTPATH_CORE_STATUS_HPP

#include <cerrno>

namespace hotpath {

class [[nodiscard]] Status {
public:
    constexpr Status() noexcept = default;
    constexpr explicit Status(int err) noexcept : err_{err} {}

    [[nodiscard]] constexpr bool ok() const noexcept { return err_ == 0; }
    [[nodiscard]] constexpr int err() const noexcept { return err_; }

    [[nodiscard]] static Status from_errno() noexcept { return Status{errno}; }

private:
    int err_{0};
};

} // namespace hotpath

#endif // HOTPATH_CORE_STATUS_HPP
