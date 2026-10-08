#ifndef HOTPATH_CORE_CONTRACT_HPP
#define HOTPATH_CORE_CONTRACT_HPP

#include <cstdio>
#include <cstdlib>

#ifndef HOTPATH_HOT_ASSERTS
#ifdef NDEBUG
#define HOTPATH_HOT_ASSERTS 0
#else
#define HOTPATH_HOT_ASSERTS 1
#endif
#endif

namespace hotpath {

[[noreturn]] [[gnu::cold]] [[gnu::noinline]]
inline void contract_fail(const char* kind, const char* expr,
                          const char* file, int line) noexcept {
    static_cast<void>(std::fprintf(stderr, "hotpath: %s failed: %s (%s:%d)\n",
                                   kind, expr, file, line));
    std::abort();
}

} // namespace hotpath

#define HOTPATH_ASSERT(cond)                                              \
    (static_cast<bool>(cond)                                              \
         ? static_cast<void>(0)                                           \
         : ::hotpath::contract_fail("assert", #cond, __FILE__, __LINE__))

#if HOTPATH_HOT_ASSERTS
#define HOTPATH_HOT_ASSERT(cond)                                              \
    (static_cast<bool>(cond)                                                  \
         ? static_cast<void>(0)                                               \
         : ::hotpath::contract_fail("hot assert", #cond, __FILE__, __LINE__))
#else
#define HOTPATH_HOT_ASSERT(cond) static_cast<void>(sizeof(static_cast<bool>(cond)))
#endif

#endif // HOTPATH_CORE_CONTRACT_HPP
