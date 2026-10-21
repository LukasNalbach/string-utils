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

#ifndef STRING_UTILS_DATA_STRUCTURES_PLAIN_BIT_VECTOR_HPP
#define STRING_UTILS_DATA_STRUCTURES_PLAIN_BIT_VECTOR_HPP

#include <cassert>

#include <iostream>
#include <sdsl/bit_vectors.hpp>
#include <sux/bits/SimpleSelect.hpp>
#include <sux/bits/SimpleSelectZero.hpp>
#include <vector>

#include <misc/utils.hpp>

/**
 * @brief queries a plain_bit_vector can support in addition to access; the values can be combined with |
 */
enum bit_vector_support_t : uint8_t {
    NO_SUPPORT = 0, // access only
    RANK_1 = 1, // rank_1 and rank_0
    SELECT_0 = 2,
    SELECT_1 = 4
};

inline constexpr bit_vector_support_t operator|(bit_vector_support_t a, bit_vector_support_t b)
{
    return bit_vector_support_t(uint8_t(a) | uint8_t(b));
}

/**
 * @brief uncompressed bit vector (sdsl::bit_vector) with optional rank support (from sdsl) and select support
 *        (from sux); the supports to build are chosen at construction (see bit_vector_support_t)
 */
class plain_bit_vector {
protected:
    // ############################# CONSTANTS #############################

    // max_log2_longwords_per_subinventory of sux::bits::SimpleSelect and SimpleSelectZero
    static constexpr int select_log2_words_per_subinventory = 3;

    // ############################# VARIABLES #############################

    sdsl::bit_vector _vec; // the bits B[0..size()-1]
    sdsl::bit_vector::rank_1_type _rank_1_support; // rank_1 support for _vec (empty if not built)
    sux::bits::SimpleSelectZero<> _select_0_support; // select_0 support for _vec (empty if not built)
    sux::bits::SimpleSelect<> _select_1_support; // select_1 support for _vec (empty if not built)

    uint64_t _num_zeros = 0; // number of zeros in _vec
    uint64_t _num_ones = 0; // number of ones in _vec

    // the supports that are built for _vec
    bit_vector_support_t _supported_queries = NO_SUPPORT;

    // ############################# INTERNAL METHODS #############################

    bool has_rank_support() const { return (_supported_queries & RANK_1) != 0; }
    bool has_select_0_support() const { return (_supported_queries & SELECT_0) != 0; }
    bool has_select_1_support() const { return (_supported_queries & SELECT_1) != 0; }

    void copy_from_other(const plain_bit_vector& other)
    {
        _vec = other._vec;
        _supported_queries = other._supported_queries;
        _rank_1_support = other._rank_1_support;
        _select_0_support = other._select_0_support;
        _select_1_support = other._select_1_support;

        _rank_1_support.set_vector(&_vec);
        _select_0_support.set_vector(_vec.data());
        _select_1_support.set_vector(_vec.data());
        advise_huge_pages_for_arrays();

        _num_ones = other._num_ones;
        _num_zeros = other._num_zeros;
    }

    void move_from_other(plain_bit_vector&& other)
    {
        _vec = std::move(other._vec);
        _supported_queries = other._supported_queries;
        _rank_1_support = std::move(other._rank_1_support);
        _select_0_support = std::move(other._select_0_support);
        _select_1_support = std::move(other._select_1_support);

        _rank_1_support.set_vector(&_vec);
        _select_0_support.set_vector(_vec.data());
        _select_1_support.set_vector(_vec.data());
        advise_huge_pages_for_arrays();

        _num_ones = other._num_ones;
        _num_zeros = other._num_zeros;

        other.detach_supports();
    }

    /**
     * @brief builds the supports selected in _supported_queries for _vec and counts its ones and zeros
     * @param num_threads number of threads to use
     */
    void build_supports(uint64_t num_threads = 1)
    {
        if (has_rank_support())
            _rank_1_support = sdsl::bit_vector::rank_1_type(&_vec);
        if (has_select_0_support())
            _select_0_support = sux::bits::SimpleSelectZero<>(
                _vec.data(), _vec.size(), select_log2_words_per_subinventory, num_threads);
        if (has_select_1_support())
            _select_1_support = sux::bits::SimpleSelect<>(
                _vec.data(), _vec.size(), select_log2_words_per_subinventory, num_threads);

        if (size() > 0) {
            if (has_rank_support()) {
                _num_ones = rank_1(size());
            } else {
                // count the ones in the words of _vec
                const uint64_t* words = _vec.data();
                uint64_t num_words = (size() + 63) / 64;
                uint64_t count = 0;

                #pragma omp parallel for num_threads(num_threads) reduction(+ : count)
                for (uint64_t i = 0; i < num_words; i++) count += __builtin_popcountll(words[i]);

                // subtract the ones in the last word that lie after the end of _vec
                uint64_t tail = size() & 63;
                if (tail != 0) count -= __builtin_popcountll(words[num_words - 1] >> tail);
                _num_ones = count;
            }

            _num_zeros = size() - _num_ones;
        }

        advise_huge_pages_for_arrays();
    }

    // advises the kernel to use huge pages for _vec and the arrays of the select supports (see advise_huge_pages)
    void advise_huge_pages_for_arrays()
    {
        auto advise_array = [](void* data, uint64_t bytes) { advise_huge_pages(data, bytes); };
        advise_huge_pages(_vec.data(), (_vec.capacity() + 7) / 8);
        _select_0_support.for_each_backing_array(advise_array);
        _select_1_support.for_each_backing_array(advise_array);
    }

    // detaches the supports from _vec and sets the numbers of ones and zeros to 0
    void detach_supports()
    {
        _rank_1_support.set_vector(nullptr);
        _select_0_support.set_vector(nullptr);
        _select_1_support.set_vector(nullptr);

        _num_ones = 0;
        _num_zeros = 0;
    }

public:
    // ############################# CONSTRUCTORS #############################

    plain_bit_vector() { build_supports(); }
    plain_bit_vector(plain_bit_vector&& other) { move_from_other(std::move(other)); }
    plain_bit_vector(const plain_bit_vector& other) { copy_from_other(other); }
    plain_bit_vector& operator=(plain_bit_vector&& other) { move_from_other(std::move(other)); return *this; }
    plain_bit_vector& operator=(const plain_bit_vector& other) { copy_from_other(other); return *this; }

    ~plain_bit_vector() { detach_supports(); }

    /**
     * @brief constructs a plain_bit_vector from a bit vector and builds the selected supports
     * @param vec a bit vector
     * @param supports the queries to support, combined with | (see bit_vector_support_t)
     * @param num_threads number of threads to use for building the supports
     */
    plain_bit_vector(const sdsl::bit_vector& vec, bit_vector_support_t supports = NO_SUPPORT,
        uint64_t num_threads = 1)
        : _supported_queries(supports)
    { _vec = vec; build_supports(num_threads); }

    plain_bit_vector(sdsl::bit_vector&& vec, bit_vector_support_t supports = NO_SUPPORT,
        uint64_t num_threads = 1)
        : _supported_queries(supports)
    { _vec = std::move(vec); build_supports(num_threads); }

    // ############################# MISC METHODS #############################

    // returns the length of the bit vector
    inline uint64_t size() const { return _vec.size(); }

    // returns whether the bit vector has length 0
    inline bool empty() const { return size() == 0; }

    // returns the number of ones
    inline uint64_t num_ones() const { return _num_ones; }

    // returns the number of zeros
    inline uint64_t num_zeros() const { return _num_zeros; }

    // returns the size of the bit vector and its supports in bytes
    uint64_t size_in_bytes() const
    {
        return sdsl::size_in_bytes(_vec) + sdsl::size_in_bytes(_rank_1_support) +
            _select_0_support.size_in_bytes() + _select_1_support.size_in_bytes();
    }

    // ############################# ACCESS METHODS #############################

    // returns whether there is a one at index i (i in [0..size()-1])
    inline bool operator[](uint64_t i) const { return _vec[i]; }

    // ############################# QUERY METHODS #############################

    // returns the number of ones before index i (i in [0..size()]; requires RANK_1)
    inline uint64_t rank_1(uint64_t i) const { assert(has_rank_support()); return _rank_1_support.rank(i); }

    // returns the number of zeros before index i (i in [0..size()]; requires RANK_1)
    inline uint64_t rank_0(uint64_t i) const { return i - rank_1(i); }

    // returns the index of the i-th one (i in [1..num_ones()]; requires SELECT_1)
    inline uint64_t select_1(uint64_t i) const
    { assert(has_select_1_support()); return _select_1_support.select(i - 1); }

    // returns the index of the i-th zero (i in [1..num_zeros()]; requires SELECT_0)
    inline uint64_t select_0(uint64_t i) const
    { assert(has_select_0_support()); return _select_0_support.selectZero(i - 1); }

    // returns the index of the next one after index i (requires RANK_1 and SELECT_1)
    inline uint64_t next_1(uint64_t i) const { return select_1(rank_1(i + 1) + 1); }

    // returns the index of the previous one before index i (requires RANK_1 and SELECT_1)
    inline uint64_t previous_1(uint64_t i) const { return select_1(rank_1(i)); }

    // returns the index of the next zero after index i (requires RANK_1 and SELECT_0)
    inline uint64_t next_0(uint64_t i) const { return select_0(rank_0(i + 1) + 1); }

    // returns the index of the previous zero before index i (requires RANK_1 and SELECT_0)
    inline uint64_t previous_0(uint64_t i) const { return select_0(rank_0(i)); }

    // ############################# SERIALIZATION METHODS #############################

    /**
     * @brief writes the bit vector and the selection of its supports to an output stream; the supports
     *        and the numbers of ones and zeros are not stored, load rebuilds them
     * @param out output stream
     */
    void serialize(std::ostream& out) const
    {
        // one byte per support (RANK_1, SELECT_0, SELECT_1); 1 <=> the support is built
        for (bit_vector_support_t support : { RANK_1, SELECT_0, SELECT_1 }) {
            char is_built = (_supported_queries & support) != 0;
            out.write(&is_built, 1);
        }

        _vec.serialize(out);
    }

    /**
     * @brief reads a serialized bit vector from an input stream and rebuilds its supports
     * @param in input stream
     */
    void load(std::istream& in)
    {
        _supported_queries = NO_SUPPORT;

        for (bit_vector_support_t support : { RANK_1, SELECT_0, SELECT_1 }) {
            char is_built;
            in.read(&is_built, 1);
            if (is_built) _supported_queries = _supported_queries | support;
        }

        _vec.load(in);
        build_supports();
    }

    std::ostream& operator>>(std::ostream& os) const { serialize(os); return os; }
    std::istream& operator<<(std::istream& is) { load(is); return is; }
};

#endif // STRING_UTILS_DATA_STRUCTURES_PLAIN_BIT_VECTOR_HPP
