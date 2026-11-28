#pragma once

#include <algorithm>
#include <vector>

#ifdef __APPLE__
#include <cstdlib>
#include <limits>
#include <new>
#include <unistd.h>
#endif

namespace photonstack {

#ifdef __APPLE__
namespace detail {
inline std::size_t pixelPageSize() {
    static const auto size = static_cast<std::size_t>(getpagesize());
    return size;
}

// Whole VM pages can be lent to Metal until a synchronous command completes.
// Small images still use the ordinary heap.
template<class T> struct PixelAllocator {
    using value_type = T;
    PixelAllocator() = default;
    template<class U> PixelAllocator(const PixelAllocator<U>&) noexcept {}
    [[nodiscard]] T* allocate(std::size_t n) {
        const auto page = pixelPageSize();
        if (n > (std::numeric_limits<std::size_t>::max() - page + 1) / sizeof(T))
            throw std::bad_array_new_length();
        const auto bytes = n * sizeof(T);
        void* storage = nullptr;
        if (bytes < page) storage = std::malloc(bytes ? bytes : 1);
        else if (posix_memalign(&storage, page, (bytes + page - 1) / page * page) != 0)
            throw std::bad_alloc();
        if (!storage) throw std::bad_alloc();
        return static_cast<T*>(storage);
    }
    void deallocate(T* ptr, std::size_t) noexcept { std::free(ptr); }
    template<class U> bool operator==(const PixelAllocator<U>&) const noexcept { return true; }
};
inline bool operator==(const std::vector<float, PixelAllocator<float>>& pixels, const std::vector<float>& other) {
    return std::equal(pixels.begin(), pixels.end(), other.begin(), other.end());
}
} // namespace detail
using PixelStorage = std::vector<float, detail::PixelAllocator<float>>;
#else
using PixelStorage = std::vector<float>;
#endif

} // namespace photonstack
