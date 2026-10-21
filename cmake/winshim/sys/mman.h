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

// Windows stand-in for <sys/mman.h> used by sux: mmap, munmap and mremap via calloc, free and realloc; it is on the
// include path only on Windows (cmake/settings.cmake)

#pragma once

#if defined(_WIN32)

#include <cstddef>
#include <cstdlib>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0
#define MAP_HUGETLB 0
#define MAP_FAILED ((void*) -1)
#define MADV_NORMAL 0
#define MADV_HUGEPAGE 0
#define MREMAP_MAYMOVE 1

// anonymous mmap: returns zero-initialized memory, as with MAP_ANONYMOUS
static inline void* mmap(void*, size_t length, int, int, int, long)
{
    void* p = std::calloc(1, length);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void* addr, size_t)
{
    std::free(addr);
    return 0;
}

static inline void* mremap(void* old_address, size_t, size_t new_size, int)
{
    return std::realloc(old_address, new_size);
}

static inline int madvise(void*, size_t, int) { return 0; }

static inline int mprotect(void*, size_t, int) { return 0; }

#endif // _WIN32
