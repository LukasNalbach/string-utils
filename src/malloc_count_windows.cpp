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

/*
 * malloc_count for Windows (the interface of external/malloc_count/malloc_count.h): counts the bytes that the program
 * has allocated and their peak. It replaces the heap functions of the C runtime in the executable: malloc, calloc,
 * realloc, free, _msize, _expand, _recalloc, their _base variants (which the static C runtime calls itself) and the
 * _aligned_* functions. The operator new of the C++ runtime calls malloc and _aligned_malloc, so every allocation of
 * the program is counted. The replacement works with the static C runtime of the MSVC ABI (the libraries of the
 * runtime are linked after this file, so their heap functions are not linked) and with MinGW-w64; there, the C runtime
 * is a DLL, whose own functions (e.g. _strdup) allocate with its heap functions, which are not counted (free accepts
 * such blocks).
 *
 * Every block starts with a header (block_header_t) in front of the address returned to the caller. As in
 * malloc_count, the counted size of a block is the size requested by the caller. Blocks of at least
 * min_huge_page_advice_bytes bytes consist of large pages (VirtualAlloc with MEM_LARGE_PAGES) if the environment
 * variable <prefix>_HUGE_PAGES is unset or > 0 and the process may use large pages (see platform::enable_large_pages);
 * the other blocks come from the heap of the process (HeapAlloc), as in the C runtime.
 */

// Windows only (the build adds this file only on Windows)
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <malloc_count.h>

#include <misc/memory.hpp>
#include <misc/platform.hpp>

namespace {

// the header in front of every block
struct block_header_t {
    uint64_t size; // the number of bytes requested by the caller (the counted size)
    uint64_t offset; // the number of bytes from the start of the allocation to the header (> 0 only if aligned)
    uint64_t capacity; // 0 for a block of the heap; else the number of bytes of the pages (VirtualAlloc)
    uint64_t sentinel; // block_sentinel
};

// marks the header of a block of this allocator (the sentinel of malloc_count)
constexpr uint64_t block_sentinel = 0xDEADC0DE;

// size of the header in bytes; a multiple of the alignment of the heap, so that the blocks keep that alignment
constexpr uint64_t header_bytes = sizeof(block_header_t);
static_assert(header_bytes % MEMORY_ALLOCATION_ALIGNMENT == 0);

// the alignment in bytes of every address returned by malloc (the alignment of HeapAlloc)
constexpr uint64_t natural_alignment = MEMORY_ALLOCATION_ALIGNMENT;

// the number of bytes allocated and not freed, their peak and the number of allocations (as in malloc_count)
std::atomic<int64_t> current_bytes { 0 };
std::atomic<int64_t> peak_bytes { 0 };
std::atomic<int64_t> num_allocations { 0 };

// the callback of malloc_count_set_callback (called after every change of current_bytes)
std::atomic<malloc_count_callback_type> callback { nullptr };
std::atomic<void*> callback_cookie { nullptr };

// adds bytes to the counted bytes and updates the peak
void count_allocation(uint64_t bytes)
{
    const int64_t current = current_bytes.fetch_add(int64_t(bytes), std::memory_order_relaxed) + int64_t(bytes);
    int64_t peak = peak_bytes.load(std::memory_order_relaxed);
    while (current > peak && !peak_bytes.compare_exchange_weak(peak, current, std::memory_order_relaxed)) { }
    num_allocations.fetch_add(1, std::memory_order_relaxed);
    if (const malloc_count_callback_type function = callback.load(std::memory_order_relaxed))
        function(callback_cookie.load(std::memory_order_relaxed), size_t(current));
}

// subtracts bytes from the counted bytes
void count_release(uint64_t bytes)
{
    const int64_t current = current_bytes.fetch_sub(int64_t(bytes), std::memory_order_relaxed) - int64_t(bytes);
    if (const malloc_count_callback_type function = callback.load(std::memory_order_relaxed))
        function(callback_cookie.load(std::memory_order_relaxed), size_t(current));
}

// returns the header of the block at data
block_header_t read_header(const void* data)
{
    block_header_t header;
    std::memcpy(&header, static_cast<const char*>(data) - header_bytes, header_bytes);
    return header;
}

// writes the header of the block at data
void write_header(void* data, const block_header_t& header)
{
    std::memcpy(static_cast<char*>(data) - header_bytes, &header, header_bytes);
}

// returns whether data is the address of a block of this allocator (else it was allocated before this allocator was
// linked in, e.g. by the C runtime of a DLL that uses the heap of the process)
bool is_own_block(const void* data)
{
    uint64_t sentinel;
    std::memcpy(&sentinel, static_cast<const char*>(data) - sizeof(sentinel), sizeof(sentinel));
    return sentinel == block_sentinel;
}

// returns the start of the allocation of the block at data
char* allocation_start(void* data, const block_header_t& header)
{
    return static_cast<char*>(data) - header_bytes - header.offset;
}

// sets errno to an error code and returns nullptr
void* fail(int error)
{
    errno = error;
    return nullptr;
}

/* returns whether blocks of at least min_huge_page_advice_bytes bytes consist of large pages: <prefix>_HUGE_PAGES is
   unset or > 0 (as in advise_huge_pages) and the process may use large pages */
bool use_large_pages()
{
#if STRING_UTILS_HUGE_PAGES
    // 1: yes, 0: no, -1: not decided yet
    static std::atomic<int> decision { -1 };
    int value = decision.load(std::memory_order_relaxed);
    if (value >= 0) return value == 1;

    // the environment is read without the C runtime, which would allocate memory
    char text[std::numeric_limits<int>::digits10 + 3] {};
    const DWORD length = GetEnvironmentVariableA(STRING_UTILS_ENV_PREFIX "_HUGE_PAGES", text, DWORD(sizeof(text)));
    const bool is_set = length > 0 && length < sizeof(text);
    int mode = 0;
    const bool is_negative = text[0] == '-';
    for (const char* digit = text + (is_negative ? 1 : 0); *digit >= '0' && *digit <= '9'; digit++) mode = 10 * mode + (*digit - '0');
    if (is_negative) mode = -mode;
    const bool wanted = !is_set || mode > 0;
    const bool possible = wanted && platform::large_page_bytes() != 0 && platform::enable_large_pages();

    if (wanted && !possible && is_set) {
        constexpr char message[] = STRING_UTILS_ENV_PREFIX "_HUGE_PAGES: the process may not use large pages (privilege \"Lock pages in "
                                   "memory\"); using pages of the default size\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, DWORD(sizeof(message) - 1), &written, nullptr);
    }

    value = possible ? 1 : 0;
    decision.store(value, std::memory_order_relaxed);
    return possible;
#else
    return false;
#endif
}

/**
 * @brief allocates a block (not counted)
 * @param size number of bytes requested by the caller
 * @param alignment the address of the block plus align_offset is a multiple of alignment (a power of two)
 * @param align_offset see alignment
 * @param zero true <=> the block is zero-initialized
 * @return the block, or nullptr if the memory is exhausted
 */
void* allocate_block(uint64_t size, uint64_t alignment, uint64_t align_offset, bool zero)
{
    // the allocation holds the header, the block and the bytes skipped to align the block
    const bool needs_padding = alignment > natural_alignment || align_offset % natural_alignment != 0;
    const uint64_t padding = needs_padding ? alignment - 1 : 0;
    if (size > std::numeric_limits<size_t>::max() - header_bytes - padding) return nullptr;
    const uint64_t bytes = header_bytes + padding + size;

    char* start;
    uint64_t capacity = 0;

    if (bytes >= min_huge_page_advice_bytes && use_large_pages()) {
        bool is_large = false;
        start = static_cast<char*>(platform::allocate_pages(bytes, true, is_large));
        const uint64_t page = is_large ? platform::large_page_bytes() : platform::page_size();
        capacity = (bytes + page - 1) / page * page;
    } else {
        start = static_cast<char*>(HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, SIZE_T(bytes)));
    }

    if (start == nullptr) return nullptr;

    // the first address after the header whose sum with align_offset is a multiple of alignment
    uint64_t address = uint64_t(start) + header_bytes;
    if (needs_padding) address = (address + align_offset + alignment - 1) / alignment * alignment - align_offset;
    void* data = reinterpret_cast<void*>(address);
    write_header(data, { size, address - header_bytes - uint64_t(start), capacity, block_sentinel });
    return data;
}

// frees the allocation of a block of this allocator (not counted)
void release_block(void* data, const block_header_t& header)
{
    char* start = allocation_start(data, header);

    if (header.capacity != 0) {
        platform::free_pages(start);
    } else {
        HeapFree(GetProcessHeap(), 0, start);
    }
}

/**
 * @brief changes the size of a block without moving it, if possible (not counted)
 * @param data the block (of this allocator)
 * @param header its header
 * @param size the new number of bytes
 * @return whether the size could be changed in place
 */
bool resize_in_place(void* data, block_header_t& header, uint64_t size)
{
    if (size > std::numeric_limits<size_t>::max() - header_bytes - header.offset) return false;
    const uint64_t bytes = header_bytes + header.offset + size;

    if (header.capacity != 0) {
        if (bytes > header.capacity) return false;
    } else {
        // a block of the heap that grows to the size of large pages is moved to large pages
        if (bytes >= min_huge_page_advice_bytes && use_large_pages()) return false;
        if (HeapReAlloc(GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, allocation_start(data, header), SIZE_T(bytes)) == nullptr)
            return false;
    }

    header.size = size;
    write_header(data, header);
    return true;
}

/**
 * @brief changes the size of a block (realloc); counts the new size after releasing the old one, as malloc_count
 * @param data the block, or nullptr
 * @param size the new number of bytes
 * @param alignment alignment of the new block (see allocate_block)
 * @param align_offset see allocate_block
 * @param zero true <=> the bytes after the old size are zero-initialized
 * @return the block, or nullptr (data is not freed then, unless size is 0)
 */
void* reallocate(void* data, uint64_t size, uint64_t alignment, uint64_t align_offset, bool zero)
{
    if (data == nullptr) {
        void* block = allocate_block(size, alignment, align_offset, zero);
        if (block == nullptr) return fail(ENOMEM);
        count_allocation(size);
        return block;
    }

    if (size == 0) {
        free(data);
        return nullptr;
    }

    // a block of the heap of the process that was not allocated here
    if (!is_own_block(data)) {
        void* block = HeapReAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, data, SIZE_T(size));
        return block == nullptr ? fail(ENOMEM) : block;
    }

    block_header_t header = read_header(data);
    const uint64_t old_size = header.size;
    const bool keeps_alignment = (uint64_t(data) + align_offset) % alignment == 0;

    if (keeps_alignment && resize_in_place(data, header, size)) {
        if (zero && size > old_size) std::memset(static_cast<char*>(data) + old_size, 0, size - old_size);
        count_release(old_size);
        count_allocation(size);
        return data;
    }

    void* block = allocate_block(size, alignment, align_offset, zero);
    if (block == nullptr) return fail(ENOMEM);
    std::memcpy(block, data, std::min(old_size, size));
    release_block(data, header);
    count_release(old_size);
    count_allocation(size);
    return block;
}

// returns whether alignment is a power of two (else errno is set as by the _aligned_* functions of the C runtime)
bool is_valid_alignment(size_t alignment)
{
    if (alignment != 0 && (alignment & (alignment - 1)) == 0) return true;
    errno = EINVAL;
    return false;
}

// returns whether num * size overflows (then errno is set to ENOMEM)
bool overflows(size_t num, size_t size)
{
    if (size == 0 || num <= std::numeric_limits<size_t>::max() / size) return false;
    errno = ENOMEM;
    return true;
}

} // namespace

// ############################# MALLOC_COUNT #############################

extern "C" size_t malloc_count_current(void) { return size_t(current_bytes.load(std::memory_order_relaxed)); }

extern "C" size_t malloc_count_peak(void) { return size_t(peak_bytes.load(std::memory_order_relaxed)); }

extern "C" void malloc_count_reset_peak(void) { peak_bytes.store(current_bytes.load(std::memory_order_relaxed), std::memory_order_relaxed); }

extern "C" size_t malloc_count_num_allocs(void) { return size_t(num_allocations.load(std::memory_order_relaxed)); }

extern "C" void malloc_count_set_callback(malloc_count_callback_type function, void* cookie)
{
    callback_cookie.store(cookie, std::memory_order_relaxed);
    callback.store(function, std::memory_order_relaxed);
}

extern "C" void malloc_count_print_status(void)
{
    std::fprintf(stderr, "malloc_count ### current %lld, peak %lld\n", (long long) current_bytes.load(), (long long) peak_bytes.load());
}

// ############################# HEAP FUNCTIONS OF THE C RUNTIME #############################

extern "C" {

void* __cdecl malloc(size_t size)
{
    void* block = allocate_block(size, natural_alignment, 0, false);
    if (block == nullptr) return fail(ENOMEM);
    count_allocation(size);
    return block;
}

void* __cdecl calloc(size_t num, size_t size)
{
    if (overflows(num, size)) return nullptr;
    void* block = allocate_block(num * size, natural_alignment, 0, true);
    if (block == nullptr) return fail(ENOMEM);
    count_allocation(num * size);
    return block;
}

void __cdecl free(void* data)
{
    if (data == nullptr) return;

    if (!is_own_block(data)) {
        HeapFree(GetProcessHeap(), 0, data);
        return;
    }

    const block_header_t header = read_header(data);
    release_block(data, header);
    count_release(header.size);
}

void* __cdecl realloc(void* data, size_t size) { return reallocate(data, size, natural_alignment, 0, false); }

size_t __cdecl _msize(void* data)
{
    if (data == nullptr) {
        errno = EINVAL;
        return std::numeric_limits<size_t>::max();
    }

    if (!is_own_block(data)) return HeapSize(GetProcessHeap(), 0, data);
    return size_t(read_header(data).size);
}

void* __cdecl _expand(void* data, size_t size)
{
    if (data == nullptr) return fail(EINVAL);

    if (!is_own_block(data))
        return HeapReAlloc(GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, data, size == 0 ? 1 : size);

    block_header_t header = read_header(data);
    const uint64_t old_size = header.size;
    if (!resize_in_place(data, header, size)) return fail(ENOMEM);
    count_release(old_size);
    count_allocation(size);
    return data;
}

void* __cdecl _recalloc(void* data, size_t num, size_t size)
{
    if (overflows(num, size)) return nullptr;
    return reallocate(data, num * size, natural_alignment, 0, true);
}

// the variants that the static C runtime calls itself
void* __cdecl _malloc_base(size_t size) { return malloc(size); }
void* __cdecl _calloc_base(size_t num, size_t size) { return calloc(num, size); }
void __cdecl _free_base(void* data) { free(data); }
void* __cdecl _realloc_base(void* data, size_t size) { return realloc(data, size); }
size_t __cdecl _msize_base(void* data) noexcept { return _msize(data); }
void* __cdecl _expand_base(void* data, size_t size) { return _expand(data, size); }
void* __cdecl _recalloc_base(void* data, size_t num, size_t size) { return _recalloc(data, num, size); }

// the aligned functions (the aligned operator new of the C++ runtime calls _aligned_malloc)
void* __cdecl _aligned_offset_malloc(size_t size, size_t alignment, size_t align_offset)
{
    if (!is_valid_alignment(alignment)) return nullptr;
    void* block = allocate_block(size, std::max<uint64_t>(alignment, natural_alignment), align_offset, false);
    if (block == nullptr) return fail(ENOMEM);
    count_allocation(size);
    return block;
}

void* __cdecl _aligned_malloc(size_t size, size_t alignment) { return _aligned_offset_malloc(size, alignment, 0); }

void* __cdecl _aligned_offset_realloc(void* data, size_t size, size_t alignment, size_t align_offset)
{
    if (!is_valid_alignment(alignment)) return nullptr;
    return reallocate(data, size, std::max<uint64_t>(alignment, natural_alignment), align_offset, false);
}

void* __cdecl _aligned_realloc(void* data, size_t size, size_t alignment)
{
    return _aligned_offset_realloc(data, size, alignment, 0);
}

void* __cdecl _aligned_offset_recalloc(void* data, size_t num, size_t size, size_t alignment, size_t align_offset)
{
    if (!is_valid_alignment(alignment) || overflows(num, size)) return nullptr;
    return reallocate(data, num * size, std::max<uint64_t>(alignment, natural_alignment), align_offset, true);
}

void* __cdecl _aligned_recalloc(void* data, size_t num, size_t size, size_t alignment)
{
    return _aligned_offset_recalloc(data, num, size, alignment, 0);
}

size_t __cdecl _aligned_msize(void* data, size_t, size_t) { return _msize(data); }

void __cdecl _aligned_free(void* data) { free(data); }

#ifdef __MINGW32__
/* MinGW declares the functions below as imported from the DLL of the C runtime (dllimport), so code compiled with its
   headers (e.g. the aligned operator new of libstdc++) calls them through the pointers __imp_<name> of the import
   library; these definitions of the pointers take precedence over the import library */
#define MALLOC_COUNT_IMPORT_POINTER(function) decltype(&function) __imp_##function = &function;
MALLOC_COUNT_IMPORT_POINTER(_msize)
MALLOC_COUNT_IMPORT_POINTER(_expand)
MALLOC_COUNT_IMPORT_POINTER(_recalloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_malloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_offset_malloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_realloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_offset_realloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_recalloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_offset_recalloc)
MALLOC_COUNT_IMPORT_POINTER(_aligned_msize)
MALLOC_COUNT_IMPORT_POINTER(_aligned_free)
#undef MALLOC_COUNT_IMPORT_POINTER
#endif

} // extern "C"

#endif // _WIN32
