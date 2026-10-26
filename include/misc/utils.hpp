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

#ifndef STRING_UTILS_MISC_UTILS_HPP
#define STRING_UTILS_MISC_UTILS_HPP

#include <cstdint>
#include <stdexcept>
#include <string>

#include <misc/aligned_table.hpp>
#include <misc/bits.hpp>
#include <misc/memory.hpp>
#include <misc/packed_integers.hpp>
#include <misc/threads.hpp>
#include <misc/type_utils.hpp>

/**
 * @brief type of array a compressed representation is built over; determines the encoding and the supported
 *        queries
 */
enum array_kind_t : uint8_t {
    // the differences of consecutive values of an integer array are encoded; supports random access
    _diff_access = 0,
    // like _diff_access, and supports range minimum, psv and nsv queries
    _diff_rmq = 1,
    // like _diff_rmq, for two integer arrays of the same length (given as a sequence of pairs) that
    // share one data structure
    _diff_pair_rmq = 2,
    // the input is encoded as it is (e.g., a byte text); supports random access and extraction
    _plain = 3
};

// selects one of the two arrays of a data structure of the kind _diff_pair_rmq
enum component_t : uint8_t { _first = 0, _second = 1 };

// a search/reading direction, or a set of them: NO_DIR is neither and BOTH_DIRS is both
enum direction_t : uint8_t {
    NO_DIR = 0,
    LEFT = 1,
    RIGHT = 2,
    BOTH_DIRS = 3
};

// key function for radix_sort: returns the value itself as a uint64_t
struct key_of_value {
    template <typename value_t>
    inline uint64_t operator()(const value_t& value) const { return uint64_t(value); }
};

// key function for radix_sort: returns the first component of a pair
struct key_of_first {
    template <typename pair_t>
    inline uint64_t operator()(const pair_t& pair) const { return uint64_t(pair.first); }
};

// key function for radix_sort: returns key[i] for an index i
template <typename value_t>
struct key_of_index {
    const value_t* key;

    template <typename index_t>
    inline uint64_t operator()(const index_t& i) const { return uint64_t(key[uint64_t(i)]); }
};

#endif // STRING_UTILS_MISC_UTILS_HPP
