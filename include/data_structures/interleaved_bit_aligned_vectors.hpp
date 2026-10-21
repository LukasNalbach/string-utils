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

#ifndef STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_HPP
#define STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_HPP

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <tuple>
#include <vector>

#include <misc/utils.hpp>
#include <misc/files.hpp>

/**
 * @brief atomically overwrites the bits selected by submask of the 64-bit word with the corresponding bits of
 *        subval (all other bits of the word are preserved); concurrent calls that write disjoint bits of the
 *        same word do not overwrite each other's bits
 * @param word reference to the aligned 64-bit word to update
 * @param submask mask selecting the bits to overwrite
 * @param subval new values for the selected bits (must be zero outside submask)
 */
inline void atomic_store_bits(uint64_t& word, uint64_t submask, uint64_t subval)
{
    uint64_t expected = __atomic_load_n(&word, __ATOMIC_RELAXED);
    uint64_t desired;

    do {
        desired = (expected & ~submask) | subval;
    } while (!__atomic_compare_exchange_n(&word, &expected, desired,
        true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

/**
 * @brief stores up to num_vectors arrays of the same length interleaved: the i-th entries of all arrays are
 *        stored in consecutive bits, each array with its own width (1..64 bits; width 0: the array is unused)
 */
class interleaved_bit_aligned_vectors {
public:
    // ############################# CONSTANTS AND TYPES #############################

    // maximum number of interleaved vectors
    static constexpr uint8_t num_vectors = 8;

protected:
    // maximum width (in bits) of a vector whose entries are read and written with a single 64-bit word
    // (an entry starts at a bit offset of at most 7 within its byte); wider entries use a uint128_t block
    static constexpr uint8_t max_word_access_width = 64 - 7;

    // number of bytes after the entries in data_vectors, so that the uint128_t reads and writes and the aligned
    // 64-bit writes of set_parallel of the last entry stay in bounds
    static constexpr uint64_t padding = std::max<uint64_t>(sizeof(uint128_t), sizeof(uint64_t));

    // (index of the byte in data_vectors that contains the first bit of an entry, offset of that bit in the byte)
    using bit_position_t = std::tuple<uint64_t, uint64_t>;

    // ############################# VARIABLES #############################

    uint64_t size_vectors = 0; // number of entries of each vector
    uint64_t capacity_vectors = 0; // number of entries of each vector that fit into data_vectors
    uint64_t width_entry = 0; // sum of the widths (in bits) of all vectors

    /* the interleaved entries: for i in [0..capacity_vectors-1] and v in [0..num_vectors-1], the bits
       [i * width_entry + offsets[v] .. i * width_entry + offsets[v] + widths[v] - 1] store entry i of vector v (bit
       j is bit j % 8 of byte j / 8); byte_size(capacity_vectors) bytes, followed by padding bytes (empty if
       capacity_vectors = 0) */
    std::vector<char> data_vectors;

    // the widths of the vectors: [0..num_vectors-1] widths[v] = width (in bits) of vector v (0: v is unused)
    std::array<uint8_t, num_vectors> widths {};

    /* the bit offsets of the vectors within an entry: [0..num_vectors-1] offsets[v] = sum of the widths of the
       vectors before v (0 if v is unused) */
    std::array<uint64_t, num_vectors> offsets {};

    /* the masks of the vectors: [0..num_vectors-1] masks[v] = 2^widths[v] - 1; aligned as uint128_t also with the
       standard library of MSVC, which packs its types to 8 bytes */
    alignas(uint128_t) std::array<uint128_t, num_vectors> masks {};

    // [0..num_vectors-1] fits_in_word[v] = 1 <=> 0 < widths[v] <= max_word_access_width
    std::array<bool, num_vectors> fits_in_word {};

    // [0..num_vectors-1] word_masks[v] = 2^widths[v] - 1 if fits_in_word[v], else 0
    std::array<uint64_t, num_vectors> word_masks {};

    // ############################# INTERNAL METHODS #############################

    /**
     * @brief sets the widths of the vectors and removes all entries
     * @param widths widths (in bits) of the vectors (0: the vector is unused)
     */
    void initialize(std::array<uint8_t, num_vectors> widths);

    /**
     * @brief returns the number of bytes of size entries, rounded up to a multiple of 8 bytes
     * @param size number of entries per vector
     */
    uint64_t byte_size(uint64_t size) const { return sizeof(uint64_t) * div_ceil(size * width_entry, 8 * sizeof(uint64_t)); }

    /**
     * @brief returns the position of the first bit of the i-th entry of the vector with index vec_idx
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @param i entry index (0 <= i < capacity_vectors)
     * @return (byte index in data_vectors, bit offset in [0..7] within that byte)
     */
    template <uint8_t vec_idx>
    inline bit_position_t bit_position(uint64_t i) const
    {
        static_assert(vec_idx < num_vectors);
        uint64_t bit_pos = i * width_entry + offsets[vec_idx];
        uint64_t byte_idx = bit_pos / 8;
        uint64_t bit_offs = bit_pos % 8;
        return {byte_idx, bit_offs};
    }

public:
    // ############################# CONSTRUCTORS #############################

    interleaved_bit_aligned_vectors() = default;

    // creates empty vectors with the given widths in bits (0: the vector is unused)
    interleaved_bit_aligned_vectors(std::array<uint8_t, num_vectors> widths) { initialize(widths); }

    // ############################# MISC METHODS #############################

    // returns the number of entries of each vector
    inline uint64_t size() const { return size_vectors; }

    // returns whether the vectors are empty
    inline bool empty() const { return size_vectors == 0; }

    // returns the size of the data structure in bytes
    uint64_t size_in_bytes() const { return sizeof(*this) + byte_size(size_vectors); }

    // returns the number of bits per entry (the sum of the widths of all vectors)
    inline uint64_t bits_per_entry() const { return width_entry; }

    // returns the width in bits of the vector with index vec_idx
    inline uint64_t width_of(uint8_t vec_idx) const { return widths[vec_idx]; }

    // returns the width in bits of the vector with index vec_idx
    template <uint8_t vec_idx>
    inline uint64_t width() const { return widths[vec_idx]; }

    /**
     * @brief returns a pointer of type T to the i-th byte of data_vectors
     * @tparam T type
     * @param i byte index in data_vectors
     */
    template <typename T = char>
    inline T* data(uint64_t i = 0)
    {
        static_assert(sizeof(T) <= 8);
        return reinterpret_cast<T*>(data_vectors.data() + i);
    }

    /**
     * @brief reserves capacity entries in all vectors; if capacity is smaller than the current capacity,
     *        nothing happens (use shrink_to_fit() to lower the capacity)
     * @param capacity number of entries
     * @param num_threads number of threads to use when copying entries (default: 1)
     */
    void reserve(uint64_t capacity, uint64_t num_threads = 1);

    /**
     * @brief resizes all vectors to size entries and sets new entries to 0
     * @param size number of entries
     * @param num_threads number of threads to use when copying entries and setting new entries to 0 (default: 1)
     */
    void resize(uint64_t size, uint64_t num_threads = 1);

    /**
     * @brief resizes all vectors to size entries without setting new entries to 0
     * @param size number of entries
     * @param num_threads number of threads to use when copying entries (default: 1)
     */
    void resize_no_init(uint64_t size, uint64_t num_threads = 1)
    {
        if (capacity_vectors < size) {
            reserve(size, num_threads);
        }

        size_vectors = size;
    }

    // resizes all vectors to 0 entries
    inline void clear() { resize(0); }

    // lowers the capacity of all vectors to their size (at least 2 entries)
    void shrink_to_fit();

    /**
     * @brief changes the width of the vector with index vec_idx; the entries of all vectors are copied
     *        to a new buffer, i.e., the old and the new encoding are in memory at the same time
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @param width new width (in bits); every value stored in the vector must fit into it; with a width
     *        of 0, the vector is removed and the other vectors are kept
     * @param num_threads number of threads to use (default: 1)
     */
    template <uint8_t vec_idx>
    void set_width(uint8_t width, uint64_t num_threads = 1);

    // ############################# ACCESS METHODS #############################

    /**
     * @brief reads the 16 bytes data_vectors[byte_idx..byte_idx+15] as a uint128_t
     * @param byte_idx byte index in data_vectors
     */
    inline uint128_t read_block(uint64_t byte_idx) const
    {
        uint128_t block;
        std::memcpy(&block, &data_vectors[byte_idx], sizeof(uint128_t));
        return block;
    }

    /**
     * @brief writes a uint128_t to the 16 bytes data_vectors[byte_idx..byte_idx+15]
     * @param byte_idx byte index in data_vectors
     * @param block the value to write
     */
    inline void write_block(uint64_t byte_idx, uint128_t block) { std::memcpy(&data_vectors[byte_idx], &block, sizeof(uint128_t)); }

    // returns the i-th entry of the vector with index 0 (0 <= i < size_vectors)
    inline uint64_t operator[](uint64_t i) const { return get<0>(i); }

    /**
     * @brief returns the i-th entry of the vector with index vec_idx
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @tparam T type to return the entry as
     * @param i entry index (0 <= i < size_vectors)
     */
    template <uint8_t vec_idx, typename T = uint64_t>
    inline T get(uint64_t i) const;

    /**
     * @brief returns the i-th entries of count consecutive vectors; they are read with a single 64-bit
     *        word or uint128_t block if their combined width (plus a bit offset of at most 7) fits into it
     * @tparam count number of consecutive vectors to read (vec_idx + count <= num_vectors)
     * @tparam vec_idx index of the first vector
     * @tparam val_t type to return the entries as
     * @param i entry index (0 <= i < size_vectors)
     * @return the entries, in the order of their vectors
     */
    template <uint64_t count, uint8_t vec_idx, typename val_t = uint64_t>
    inline std::array<val_t, count> get_multiple(uint64_t i) const;

    /**
     * @brief prefetches the cache line containing the first byte of the i-th entry
     * @param i entry index (0 <= i < size_vectors)
     */
    inline void prefetch(uint64_t i) const
    { __builtin_prefetch(&data_vectors[(i * width_entry) / 8]); }

    /**
     * @brief sets the i-th entry of the vector with index vec_idx to v
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @tparam T type of the value to store
     * @param i entry index (0 <= i < capacity_vectors)
     * @param v value to store
     */
    template <uint8_t vec_idx, typename T = uint64_t>
    inline void set(uint64_t i, T v);

    /**
     * @brief sets the i-th entry of the vector with index vec_idx to v; unlike set(), this may be called
     *        concurrently for distinct entries (or distinct vectors of the same entry), even if their bits share
     *        a byte; concurrent writes to the same entry of the same vector are not supported
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @tparam T type of the value to store
     * @param i entry index (0 <= i < capacity_vectors)
     * @param v value to store
     */
    template <uint8_t vec_idx, typename T = uint64_t>
    inline void set_parallel(uint64_t i, T v);

    /**
     * @brief appends an entry to the vectors
     * @param vals [0..num_vectors-1] vals[v] = value of vector v
     */
    inline void emplace_back(std::array<uint64_t, num_vectors> vals);

    /**
     * @brief appends an entry to the vectors with the value val in the vector with index vec_idx and 0 in
     *        the other vectors
     * @tparam vec_idx vector index (0 <= vec_idx < num_vectors)
     * @tparam T type of the value to append
     * @param val value to append
     */
    template <uint8_t vec_idx = 0, typename T = uint64_t>
    inline void emplace_back(T val);

    // ############################# SERIALIZATION METHODS #############################

    /**
     * @brief serializes the interleaved vectors to an output stream
     * @param out output stream
     */
    void serialize(std::ostream& out) const;

    /**
     * @brief loads the interleaved vectors from an input stream
     * @param in input stream
     */
    void load(std::istream& in);

    std::ostream& operator>>(std::ostream& os) const { serialize(os); return os; }
    std::istream& operator<<(std::istream& is) { load(is); return is; }
};

#include "interleaved_bit_aligned_vectors.tpp"

#endif // STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_HPP
