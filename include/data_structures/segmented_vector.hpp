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

#ifndef STRING_UTILS_DATA_STRUCTURES_SEGMENTED_VECTOR_HPP
#define STRING_UTILS_DATA_STRUCTURES_SEGMENTED_VECTOR_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

#include <misc/utils.hpp>
#include <misc/portable_layout.hpp>

/**
 * @brief returns the base-2 logarithm of the default block length (in elements) of a segmented_vector,
 *        i.e., the smallest value in [10..30] with which a block occupies at least 2^20 bytes
 * @tparam val_t element type
 */
template <typename val_t>
constexpr uint8_t default_log_block_length()
{
    uint8_t log = 10;
    while (log < 30 && (uint64_t(1) << log) * sizeof(val_t) < (uint64_t(1) << 20)) log++;
    return log;
}

// returns the base-2 logarithm of the smallest number of elements of type val_t that fill a huge page
template <typename val_t>
constexpr uint8_t log_huge_page_block_length()
{
    uint8_t log = 0;
    while (log < 30 && (uint64_t(1) << log) * sizeof(val_t) < huge_page_bytes) log++;
    return log;
}

/**
 * @brief number of threads that write a segmented_vector
 */
enum vector_writers_t : uint8_t {
    _one_writer = 0, // one thread; the table stores plain block pointers and no lock is used
    _many_writers = 1 // several threads; the table stores atomic block pointers, blocks are allocated under a lock
};

/**
 * @brief vector stored in blocks of 2^log_block_length elements; the blocks are allocated on demand and never
 *        moved, i.e., references to elements stay valid when the vector grows
 *
 * With _many_writers, the table of block pointers has one entry per block of the largest size the vector may reach
 * (given to the constructor or to set_max_size; 0 until then) and is allocated together with the first block (zero-filled by calloc, so
 * only its pages that are written occupy memory); a thread that accesses an unallocated block allocates it under a
 * lock. An index of max_size() or more throws std::length_error. With _one_writer, the table grows with the number
 * of blocks.
 *
 * @tparam val_t element type; must be trivially default constructible (the blocks are allocated uninitialized)
 * @tparam writers number of threads that write the vector (see vector_writers_t)
 * @tparam log_block_length base-2 logarithm of the block length (see default_log_block_length and
 *         log_huge_page_block_length)
 * @tparam huge_pages whether the blocks start at huge page boundaries and are advised to use huge pages (for
 *         vectors accessed at random positions)
 */
template <typename val_t, vector_writers_t writers = _many_writers,
    uint8_t log_block_length = default_log_block_length<val_t>(), bool huge_pages = false>
class segmented_vector {
    static_assert(std::is_trivially_default_constructible_v<val_t>,
        "segmented_vector requires a trivially default constructible element type");

    // ############################# CONSTANTS AND TYPES #############################

    static constexpr bool has_many_writers = writers == _many_writers;
    static constexpr uint64_t block_length = uint64_t(1) << log_block_length; // number of elements per block
    static constexpr uint64_t offset_mask = block_length - 1; // i & offset_mask is the index of i in its block

    // ############################# VARIABLES #############################

    /* the pointers to the blocks (only with _one_writer): [0..block_ptrs.size()-1] block_ptrs[b] = pointer to the
       elements [b * block_length .. (b + 1) * block_length - 1], nullptr if block b is not allocated; as many
       entries as the largest allocated block index + 1 */
    std::vector<val_t*> block_ptrs;

    /* the pointers to the blocks (only with _many_writers): [0..num_table_entries-1] as block_ptrs, read and written
       through std::atomic_ref; nullptr until the first block is allocated, then max_blocks entries */
    val_t** table = nullptr;
    // number of entries of table (0 while it is not allocated; written with release after table)
    std::atomic<uint64_t> num_table_entries { 0 };
    uint64_t max_blocks = 0; // number of entries of table once it is allocated (only _many_writers)

    // locked while a block is allocated (only with _many_writers)
    STRING_UTILS_NO_UNIQUE_ADDRESS std::conditional_t<has_many_writers, std::mutex, std::monostate> allocation_mutex;

    uint64_t _size = 0; // number of elements

    // ############################# INTERNAL METHODS #############################

    // allocates an uninitialized block
    static val_t* new_block()
    {
        if constexpr (!huge_pages) {
            return new val_t[block_length];
        } else {
            void* block = ::operator new[](block_length * sizeof(val_t), std::align_val_t(huge_page_bytes));
            advise_huge_pages(block, block_length * sizeof(val_t), huge_page_bytes);
            return static_cast<val_t*>(block);
        }
    }

    // deallocates a block
    static void delete_block(val_t* block)
    {
        if constexpr (!huge_pages) delete[] block;
        else ::operator delete[](block, std::align_val_t(huge_page_bytes));
    }

    // returns the number of entries of the table of block pointers
    inline uint64_t num_blocks_in_table() const
    {
        if constexpr (has_many_writers) return num_table_entries.load(std::memory_order_acquire);
        else return block_ptrs.size();
    }

    // returns the pointer to the block with index block (block < num_blocks_in_table())
    inline val_t* block_ptr(uint64_t block) const
    {
        if constexpr (has_many_writers) return std::atomic_ref<val_t*>(table[block]).load(std::memory_order_acquire);
        else return block_ptrs[block];
    }

    // sets the pointer to the block with index block (block < num_blocks_in_table())
    inline void set_block_ptr(uint64_t block, val_t* ptr)
    {
        if constexpr (has_many_writers) std::atomic_ref<val_t*>(table[block]).store(ptr, std::memory_order_release);
        else block_ptrs[block] = ptr;
    }

    // allocates a zero-filled table of entries block pointers (only with _many_writers)
    static val_t** new_table(uint64_t entries)
    {
        val_t** allocated = static_cast<val_t**>(std::calloc(entries, sizeof(val_t*)));
        if (allocated == nullptr) throw std::bad_alloc();
        return allocated;
    }

    // deallocates the allocated blocks with indices in [beg..end-1] (none if beg >= end; end <= num_blocks_in_table())
    void delete_blocks(uint64_t beg, uint64_t end)
    {
        for (uint64_t block = beg; block < end; block++) {
            val_t* ptr = block_ptr(block);
            if (ptr != nullptr) { delete_block(ptr); set_block_ptr(block, nullptr); }
        }
    }

    // moves the blocks of other into this vector and leaves other empty (this vector has no table)
    void move_from(segmented_vector& other)
    {
        if constexpr (!has_many_writers) {
            block_ptrs = std::move(other.block_ptrs);
            other.block_ptrs.clear();
        } else {
            table = other.table;
            num_table_entries.store(other.num_table_entries.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            max_blocks = other.max_blocks;
            other.table = nullptr;
            other.num_table_entries.store(0, std::memory_order_relaxed);
        }

        _size = other._size;
        other._size = 0;
    }

    // allocates the block with index block, if it is not allocated yet, and returns it
    val_t* allocate(uint64_t block)
    {
        if constexpr (!has_many_writers) {
            while (block_ptrs.size() <= block) block_ptrs.emplace_back(nullptr);
            if (block_ptrs[block] == nullptr) block_ptrs[block] = new_block();
            return block_ptrs[block];
        } else {
            if (block >= max_blocks) [[unlikely]] {
                throw std::length_error("segmented_vector: index beyond the maximum size of the vector");
            }

            std::lock_guard<std::mutex> guard(allocation_mutex);

            // the table is allocated together with the first block
            if (table == nullptr) {
                table = new_table(max_blocks);
                num_table_entries.store(max_blocks, std::memory_order_release);
            }

            val_t* ptr = std::atomic_ref<val_t*>(table[block]).load(std::memory_order_relaxed);
            if (ptr != nullptr) return ptr;

            val_t* allocated = new_block();
            set_block_ptr(block, allocated);
            return allocated;
        }
    }

    // returns the block containing index i and allocates it, if it is not allocated yet
    inline val_t* block_of(uint64_t i)
    {
        uint64_t block = i >> log_block_length;
        if (num_blocks_in_table() <= block) return allocate(block);
        val_t* ptr = block_ptr(block);
        return ptr != nullptr ? ptr : allocate(block);
    }

    // returns the block containing index i (nullptr if it is not allocated)
    inline const val_t* block_of(uint64_t i) const
    {
        if (num_blocks_in_table() == 0) [[unlikely]] return nullptr;
        assert((i >> log_block_length) < num_blocks_in_table());
        return block_ptr(i >> log_block_length);
    }

public:
    // ############################# CONSTRUCTORS #############################

    segmented_vector() = default;

    /**
     * @brief constructs an empty vector
     * @param max_size largest number of elements the vector may reach (only with _many_writers; see set_max_size)
     */
    explicit segmented_vector(uint64_t max_size) { set_max_size(max_size); }

    ~segmented_vector() { clear(); }

    segmented_vector(const segmented_vector&) = delete;
    segmented_vector& operator=(const segmented_vector&) = delete;

    segmented_vector(segmented_vector&& other) noexcept { move_from(other); }

    segmented_vector& operator=(segmented_vector&& other) noexcept
    {
        if (this != &other) { clear(); move_from(other); }
        return *this;
    }

    // ############################# MISC METHODS #############################

    // returns the number of elements (the number of appended elements, or the value passed to set_size)
    inline uint64_t size() const { return _size; }

    // returns whether the vector is empty
    inline bool empty() const { return _size == 0; }

    // returns the largest number of elements the vector may reach (unbounded with _one_writer)
    inline uint64_t max_size() const
    {
        if constexpr (has_many_writers) return max_blocks * block_length;
        else return std::numeric_limits<uint64_t>::max();
    }

    /**
     * @brief sets the largest number of elements the vector may reach (only with _many_writers; with _one_writer
     *        the vector is unbounded); must not be called while other threads access the vector
     * @param max_size largest number of elements; at least the number of elements in allocated blocks
     */
    void set_max_size(uint64_t max_size)
    {
        if constexpr (has_many_writers) {
            const uint64_t new_max_blocks = std::max<uint64_t>(1, div_ceil(max_size, block_length));
            if (new_max_blocks == max_blocks) return;

            // an allocated table is copied into a table of the new size
            if (table != nullptr) {
                for (uint64_t block = new_max_blocks; block < num_blocks_in_table(); block++) {
                    if (block_ptr(block) != nullptr) [[unlikely]] {
                        throw std::length_error("segmented_vector: the maximum size is below an allocated block");
                    }
                }

                val_t** resized = new_table(new_max_blocks);
                std::copy_n(table, std::min(num_blocks_in_table(), new_max_blocks), resized);
                std::free(table);
                table = resized;
                num_table_entries.store(new_max_blocks, std::memory_order_release);
            }

            max_blocks = new_max_blocks;
        }
    }

    // sets the number of elements to count (to be called after the elements [0..count-1] have been written
    // with operator[])
    inline void set_size(uint64_t count) { _size = count; }

    // allocates the blocks containing the first count elements
    void reserve(uint64_t count)
    {
        for (uint64_t block = 0; block * block_length < count; block++)
            if (num_blocks_in_table() <= block || block_ptr(block) == nullptr) allocate(block);
    }

    // deallocates all blocks and the table and sets the size to 0 (the maximum size stays)
    void clear()
    {
        delete_blocks(0, num_blocks_in_table());

        if constexpr (!has_many_writers) {
            block_ptrs.clear();
            block_ptrs.shrink_to_fit();
        } else {
            std::free(table);
            table = nullptr;
            num_table_entries.store(0, std::memory_order_relaxed);
        }

        _size = 0;
    }

    /**
     * @brief deallocates the blocks that contain only elements before index; the elements in these
     *        blocks must not be accessed afterwards, size() does not change
     * @param index index of the first element that is still needed
     */
    void free_blocks_before(uint64_t index)
    {
        delete_blocks(0, std::min<uint64_t>(index >> log_block_length, num_blocks_in_table()));
    }

    // deallocates the blocks after the last block containing an element
    void shrink_to_fit()
    {
        delete_blocks((_size + block_length - 1) >> log_block_length, num_blocks_in_table());
    }

    // returns the size in bytes
    uint64_t size_in_bytes() const
    {
        uint64_t num_allocated_blocks = 0;

        for (uint64_t block = 0; block < num_blocks_in_table(); block++) {
            if (block_ptr(block) != nullptr) num_allocated_blocks++;
        }

        return num_allocated_blocks * block_length * sizeof(val_t) + num_blocks_in_table() * sizeof(val_t*);
    }

    // ############################# ACCESS METHODS #############################

    /**
     * @brief returns a reference to the element at index i
     * @param i index; with _many_writers, the block containing i is allocated if it is not allocated yet
     *        (i may be >= size()); with _one_writer, the block must be allocated (see push_back and reserve)
     */
    inline val_t& operator[](uint64_t i)
    {
        if constexpr (has_many_writers) return block_of(i)[i & offset_mask];
        else return block_ptrs[i >> log_block_length][i & offset_mask];
    }

    /**
     * @brief returns the element at index i
     * @param i index of an element that has been written
     */
    inline const val_t& operator[](uint64_t i) const
    {
        if constexpr (has_many_writers) return block_of(i)[i & offset_mask];
        else return block_ptrs[i >> log_block_length][i & offset_mask];
    }

    // returns the last element
    inline val_t& back() { return (*this)[_size - 1]; }

    // returns the last element
    inline const val_t& back() const { return (*this)[_size - 1]; }

    /**
     * @brief returns whether the elements at the indices [index..index+count-1] are equal to values[0..count-1]
     * @param index index of the first element to compare
     * @param values elements to compare with
     * @param count number of elements to compare
     */
    inline bool equal_at(uint64_t index, const val_t* values, uint64_t count) const
    {
        for (uint64_t i = 0; i < count; i++) if (!((*this)[index + i] == values[i])) return false;
        return true;
    }

    // ############################# APPEND AND REMOVE METHODS #############################

    // appends an element
    inline void push_back(const val_t& value)
    {
        if constexpr (!has_many_writers) {
            if ((_size & offset_mask) == 0) [[unlikely]] allocate(_size >> log_block_length);
        }

        (*this)[_size] = value;
        _size++;
    }

    // appends the elements values[0..count-1]
    inline void append(const val_t* values, uint64_t count)
    {
        for (uint64_t i = 0; i < count; i++) push_back(values[i]);
    }

    // removes the last element (its block stays allocated)
    inline void pop_back() { _size--; }
};

#endif // STRING_UTILS_DATA_STRUCTURES_SEGMENTED_VECTOR_HPP
