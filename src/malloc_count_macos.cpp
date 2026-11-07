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

/**
 * @file malloc_count_macos.cpp
 * @brief the operators new and delete for malloc_count on macOS: the libraries of macOS are linked with two-level
 *        namespaces, so the operator new of libc++ calls the malloc of the system, not the malloc of malloc_count.c in
 *        the executable. The operators replaced here (the executable's definitions replace those of libc++ in all
 *        images) call malloc, aligned_alloc and free, which malloc_count counts.
 */

// macOS only (the build adds this file only on macOS)
#ifdef __APPLE__

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// allocates size bytes (at least 1) aligned to align (0: the alignment of malloc); returns nullptr on failure
void* allocate(std::size_t size, std::size_t align) noexcept
{
    if (size == 0) size = 1;
    if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) return std::malloc(size);
    // aligned_alloc requires a size that is a multiple of the alignment
    return std::aligned_alloc(align, (size + align - 1) / align * align);
}

// allocates as operator new: calls the new-handler until the allocation succeeds or there is none
void* allocate_or_throw(std::size_t size, std::size_t align)
{
    for (;;) {
        if (void* ptr = allocate(size, align)) return ptr;
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

} // namespace

void* operator new(std::size_t size) { return allocate_or_throw(size, 0); }
void* operator new[](std::size_t size) { return allocate_or_throw(size, 0); }
void* operator new(std::size_t size, std::align_val_t align) { return allocate_or_throw(size, std::size_t(align)); }
void* operator new[](std::size_t size, std::align_val_t align) { return allocate_or_throw(size, std::size_t(align)); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, 0); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, 0); }
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return allocate(size, std::size_t(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return allocate(size, std::size_t(align));
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { std::free(ptr); }

#endif // __APPLE__
