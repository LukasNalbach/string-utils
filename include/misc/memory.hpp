/**
 * part of LukasNalbach/string-utils
 *
 * MIT License
 *
 * Copyright (c) Lukas Nalbach
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef STRING_UTILS_MISC_MEMORY_HPP
#define STRING_UTILS_MISC_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <compare>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <malloc_count.h>

#include <misc/platform.hpp>

// set by the build: 1 if malloc_count is linked, 0 if cmake/malloc_count_stub.c is linked instead
#ifndef STRING_UTILS_USE_MALLOC_COUNT
#define STRING_UTILS_USE_MALLOC_COUNT 1
#endif

// true <=> malloc_count is linked
inline constexpr bool is_malloc_count_linked = STRING_UTILS_USE_MALLOC_COUNT != 0;

/**
 * @brief resizes str to size; std::string sets new characters to 0 (it has no resize without initialization)
 * @param str the string to resize
 * @param size the new size
 */
inline void resize_no_init(std::string& str, uint64_t size) { str.resize(size); }

// tag for no_init_allocator::construct: constructing an element from it default-initializes the element
struct default_init_t {
    // only needed to compile the branches of std::vector::insert that do not construct
    template <typename T> operator T() const noexcept(std::is_nothrow_default_constructible_v<T>) { return T(); }
};

/**
 * @brief allocator that behaves like std::allocator, except that constructing an element from default_init_t
 *        default-initializes it, i.e., a trivial value is left uninitialized (see resize_no_init)
 * @tparam T element type
 */
template <typename T>
struct no_init_allocator {
    using value_type = T;

    no_init_allocator() = default;
    template <typename U> constexpr no_init_allocator(const no_init_allocator<U>&) noexcept { }

    T* allocate(size_t num) { return std::allocator<T>().allocate(num); }
    void deallocate(T* ptr, size_t num) noexcept { std::allocator<T>().deallocate(ptr, num); }

    template <typename U, typename... args_t>
    void construct(U* ptr, args_t&&... args)
    {
        if constexpr (std::is_same_v<std::tuple<std::remove_cvref_t<args_t>...>, std::tuple<default_init_t>>) {
            if constexpr (std::is_trivially_default_constructible_v<U>) {
                ::new (static_cast<void*>(ptr)) U;
            } else {
                // e.g. std::pair or std::tuple of trivial values, whose default constructors set the values to 0;
                // memmove implicitly creates an object of such an implicit-lifetime type without writing to it
                static_assert(std::is_trivially_copy_constructible_v<U> && std::is_trivially_destructible_v<U>);
                std::memmove(static_cast<void*>(ptr), static_cast<const void*>(ptr), sizeof(U));
            }
        } else {
            ::new (static_cast<void*>(ptr)) U(std::forward<args_t>(args)...);
        }
    }

    template <typename U> bool operator==(const no_init_allocator<U>&) const noexcept { return true; }
};

// std::vector that resize_no_init can resize without initializing the new elements; otherwise a plain std::vector
template <typename T>
using no_init_vector = std::vector<T, no_init_allocator<T>>;

// random access iterator over copies of default_init_t; inserting such a range into a no_init_vector default-initializes
// the new elements
class default_init_iterator {
    int64_t _pos = 0;

public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = default_init_t;
    using difference_type = int64_t;
    using pointer = const default_init_t*;
    using reference = default_init_t;

    default_init_iterator() = default;
    explicit default_init_iterator(int64_t pos) noexcept : _pos { pos } { }

    default_init_t operator*() const noexcept { return { }; }
    default_init_t operator[](difference_type) const noexcept { return { }; }
    default_init_iterator& operator++() noexcept { _pos++; return *this; }
    default_init_iterator operator++(int) noexcept { return default_init_iterator(_pos++); }
    default_init_iterator& operator--() noexcept { _pos--; return *this; }
    default_init_iterator operator--(int) noexcept { return default_init_iterator(_pos--); }
    default_init_iterator& operator+=(difference_type num) noexcept { _pos += num; return *this; }
    default_init_iterator& operator-=(difference_type num) noexcept { _pos -= num; return *this; }
    default_init_iterator operator+(difference_type num) const noexcept { return default_init_iterator(_pos + num); }
    default_init_iterator operator-(difference_type num) const noexcept { return default_init_iterator(_pos - num); }
    difference_type operator-(const default_init_iterator& other) const noexcept { return _pos - other._pos; }
    auto operator<=>(const default_init_iterator&) const noexcept = default;
};

/**
 * @brief resizes vec to size without initializing newly added elements
 * @tparam val_t element type (a trivial type, or a std::pair or std::tuple of trivial types)
 * @param vec the vector to resize
 * @param size the new size
 */
template <typename val_t>
inline void resize_no_init(no_init_vector<val_t>& vec, uint64_t size)
{
    if (size <= vec.size()) {
        vec.resize(size);
    } else {
        vec.insert(vec.end(), default_init_iterator(vec.size()), default_init_iterator(size));
    }
}

/**
 * @brief resizes a container whose new elements are overwritten before they are read: without initializing them if
 *        resize_no_init supports the container (e.g. a no_init_vector), otherwise with resize
 * @tparam container_t type of the container
 * @param container the container to resize
 * @param size the new size
 */
template <typename container_t>
inline void resize_for_overwrite(container_t& container, uint64_t size)
{
    if constexpr (requires { resize_no_init(container, size); }) {
        resize_no_init(container, size);
    } else {
        container.resize(size);
    }
}

// the size of a cache line in bytes
inline constexpr uint64_t cache_line_bytes = 64;

/**
 * @brief stores the data of one thread in its own cache lines (aligned to a cache line, and padded to a multiple of
 *        it), so that threads that write their data at the same time do not share a cache line
 * @tparam data_t type of the data
 */
template <typename data_t>
struct alignas(cache_line_bytes) cache_line_padded {
    data_t data;
};

/**
 * @brief allocator that aligns the allocated memory to a cache line (cache_line_bytes) and default-initializes the
 *        elements, i.e., resizing a vector of integers does not zero the new elements
 * @tparam T element type
 */
template <typename T>
struct cache_aligned_no_init_allocator {
    using value_type = T;
    static constexpr size_t alignment = cache_line_bytes;

    cache_aligned_no_init_allocator() = default;
    template <typename U> cache_aligned_no_init_allocator(const cache_aligned_no_init_allocator<U>&) { }
    template <typename U> struct rebind { using other = cache_aligned_no_init_allocator<U>; };

    T* allocate(size_t num) { return static_cast<T*>(::operator new(num * sizeof(T), std::align_val_t(alignment))); }
    void deallocate(T* ptr, size_t num) { ::operator delete(ptr, num * sizeof(T), std::align_val_t(alignment)); }
    template <typename U> void construct(U* ptr) noexcept { ::new (static_cast<void*>(ptr)) U; }
    template <typename U, typename... args_t> void construct(U* ptr, args_t&&... args) { ::new (static_cast<void*>(ptr)) U(std::forward<args_t>(args)...); }
    bool operator==(const cache_aligned_no_init_allocator&) const { return true; }
    bool operator!=(const cache_aligned_no_init_allocator&) const { return false; }
};

// if STRING_UTILS_HUGE_PAGES is defined as 0, advise_huge_pages does nothing
#ifndef STRING_UTILS_HUGE_PAGES
#define STRING_UTILS_HUGE_PAGES 1
#endif

// default minimum size in bytes of a memory range for which advise_huge_pages advises the kernel
inline constexpr uint64_t min_huge_page_advice_bytes = 4 * 1024 * 1024;
// size in bytes of a transparent huge page
inline constexpr uint64_t huge_page_bytes = uint64_t(1) << 21;

/**
 * @brief advises the kernel to use huge pages for a memory range of at least min_bytes bytes (does nothing without
 *        platform::has_transparent_huge_pages). The environment variable <prefix>_HUGE_PAGES changes the advice at run
 *        time: unset or > 0: use huge pages (MADV_HUGEPAGE), 0: no advice, < 0: do not use huge pages
 *        (MADV_NOHUGEPAGE).
 * @param data start of the range
 * @param bytes size of the range in bytes
 * @param min_bytes smallest range that is advised
 */
inline void advise_huge_pages([[maybe_unused]] void* data, [[maybe_unused]] uint64_t bytes,
    [[maybe_unused]] uint64_t min_bytes = min_huge_page_advice_bytes)
{
#if STRING_UTILS_HUGE_PAGES
    if constexpr (!platform::has_transparent_huge_pages) return;
    static const int mode = [] {
        const char* value = std::getenv(STRING_UTILS_ENV_PREFIX "_HUGE_PAGES");
        return value == nullptr ? 1 : std::atoi(value);
    }();

    if (mode == 0 || bytes < min_bytes) return;

    // extend the range to page boundaries
    uint64_t page = platform::page_size();
    uintptr_t beg = uintptr_t(data) & ~(page - 1);
    uintptr_t end = (uintptr_t(data) + bytes + page - 1) & ~(page - 1);
    platform::advise_huge_pages(reinterpret_cast<void*>(beg), end - beg, mode > 0);
#endif
}

#endif // STRING_UTILS_MISC_MEMORY_HPP
