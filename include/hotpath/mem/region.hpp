#ifndef HOTPATH_MEM_REGION_HPP
#define HOTPATH_MEM_REGION_HPP

#include <cstddef>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/sys/memory.hpp"
#include "hotpath/sys/numa.hpp"

namespace hotpath::mem {

class Region {
public:
    Region() noexcept = default;

    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    Region(Region&& other) noexcept
        : map_{other.map_}, prefaulted_{other.prefaulted_} {
        other.map_ = sys::Mapping{};
        other.prefaulted_ = false;
    }

    Region& operator=(Region&& other) noexcept {
        if (this != &other) {
            release();
            map_ = other.map_;
            prefaulted_ = other.prefaulted_;
            other.map_ = sys::Mapping{};
            other.prefaulted_ = false;
        }
        return *this;
    }

    ~Region() { release(); }

    [[nodiscard]] Status map(std::size_t len, sys::PageKind kind) noexcept {
        HOTPATH_ASSERT(map_.addr == nullptr);
        const sys::Mapping m = sys::map_anonymous(len, kind);
        if (!m.status.ok()) {
            return m.status;
        }
        map_ = m;
        return Status{};
    }

    [[nodiscard]] Status bind_to_current_node() noexcept {
        HOTPATH_ASSERT(map_.addr != nullptr);
        HOTPATH_ASSERT(!prefaulted_);
        const sys::NodeResult node = sys::current_node();
        if (!node.status.ok()) {
            return node.status;
        }
        return sys::bind_to_node(map_.addr, map_.len, node.node);
    }

    void prefault() noexcept {
        HOTPATH_ASSERT(map_.addr != nullptr);
        sys::prefault(map_);
        prefaulted_ = true;
    }

    [[nodiscard]] void* data() const noexcept { return map_.addr; }
    [[nodiscard]] std::size_t size() const noexcept { return map_.len; }
    [[nodiscard]] sys::PageKind page_kind() const noexcept { return map_.kind; }
    [[nodiscard]] bool mapped() const noexcept { return map_.addr != nullptr; }
    [[nodiscard]] bool prefaulted() const noexcept { return prefaulted_; }

private:
    void release() noexcept {
        if (map_.addr != nullptr) {
            const Status st = sys::unmap(map_);
            HOTPATH_ASSERT(st.ok());
            prefaulted_ = false;
        }
    }

    sys::Mapping map_{};
    bool prefaulted_{false};
};

} // namespace hotpath::mem

#endif // HOTPATH_MEM_REGION_HPP
