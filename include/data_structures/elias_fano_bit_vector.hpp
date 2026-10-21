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

#ifndef STRING_UTILS_DATA_STRUCTURES_ELIAS_FANO_BIT_VECTOR_HPP
#define STRING_UTILS_DATA_STRUCTURES_ELIAS_FANO_BIT_VECTOR_HPP

#include <cstdint>
#include <istream>
#include <sstream>
#include <ostream>
#include <vector>

#include <sdsl/int_vector.hpp>
#include <sux/bits/EliasFano.hpp>

#include <misc/utils.hpp>

/**
 * @brief bit vector that stores the positions of its ones with Elias-Fano coding, with rank and select support
 *        (wrapper of sux::bits::EliasFano)
 */
class elias_fano_bit_vector {
protected:
    // ############################# VARIABLES #############################

    sux::bits::EliasFano<> _ef; // Elias-Fano coding of the positions of the ones (empty if there are none)
    uint64_t _size = 0; // length of the bit vector
    uint64_t _num_ones = 0; // number of ones

    // ############################# INTERNAL METHODS #############################

    // copies other by serializing and loading it (sux::bits::EliasFano is not copyable)
    void copy_from_other(const elias_fano_bit_vector& other)
    {
        _size = other._size;
        _num_ones = other._num_ones;

        if (_num_ones > 0) {
            std::stringstream buf;
            other._ef.serialize(buf);
            _ef.load(buf);
            advise_huge_pages_for_arrays();
        }
    }

    // advises the kernel to use huge pages for the arrays of _ef (see advise_huge_pages)
    void advise_huge_pages_for_arrays()
    {
        _ef.for_each_backing_array([](void* data, uint64_t bytes) { advise_huge_pages(data, bytes); });
    }

    // returns the i-th 64-bit word of a bit vector of num_bits bits, with the bits after position num_bits - 1 cleared
    static inline uint64_t word_without_tail(const uint64_t* words, uint64_t i, uint64_t num_bits)
    {
        uint64_t first_bit = i * 64;
        if (first_bit + 64 <= num_bits) return words[i];
        return words[i] & ((uint64_t(1) << (num_bits - first_bit)) - 1);
    }

public:
    // ############################# CONSTRUCTORS #############################

    elias_fano_bit_vector() = default;
    elias_fano_bit_vector(elias_fano_bit_vector&&) = default;
    elias_fano_bit_vector& operator=(elias_fano_bit_vector&&) = default;
    elias_fano_bit_vector(const elias_fano_bit_vector& other) { copy_from_other(other); }
    elias_fano_bit_vector& operator=(const elias_fano_bit_vector& other) { copy_from_other(other); return *this; }

    /**
     * @brief builds the bit vector from the positions of its ones
     * @tparam alloc_t allocator type of positions
     * @param positions positions of the ones in increasing order
     * @param universe length of the bit vector (larger than each position)
     * @param num_threads number of threads to use
     */
    template <typename alloc_t>
    elias_fano_bit_vector(const std::vector<uint64_t, alloc_t>& positions, uint64_t universe,
        uint64_t num_threads = 1)
        : _size(universe), _num_ones(positions.size())
    {
        if (!positions.empty()) {
            _ef = sux::bits::EliasFano<>([&](uint64_t i) { return uint64_t(positions[i]); }, positions.size(),
                universe, num_threads);
            advise_huge_pages_for_arrays();
        }
    }

    /**
     * @brief builds the bit vector from the positions of its ones, which are returned by a function
     * @tparam read_fnc_t type of the function read
     * @param read function returning the position of the (i+1)-th one, for i in [0..num_positions-1]
     *        (increasing in i); called concurrently if num_threads > 1
     * @param num_positions number of ones
     * @param universe length of the bit vector (larger than each position)
     * @param num_threads number of threads to use
     */
    template <typename read_fnc_t>
    elias_fano_bit_vector(read_fnc_t read, uint64_t num_positions, uint64_t universe, uint64_t num_threads = 1)
        : _size(universe), _num_ones(num_positions)
    {
        if (num_positions > 0) {
            _ef = sux::bits::EliasFano<>(read, num_positions, universe, num_threads);
            advise_huge_pages_for_arrays();
        }
    }

    /**
     * @brief builds the bit vector from an uncompressed bit vector
     * @param bits a bit vector
     * @param num_threads number of threads to use
     */
    elias_fano_bit_vector(const sdsl::bit_vector& bits, uint64_t num_threads = 1) : _size(bits.size())
    {
        // split the words of bits into chunks and count the ones in each chunk
        const uint64_t* words = bits.data();
        uint64_t num_words = (bits.size() + 63) / 64;
        uint64_t num_chunks = num_threads <= 1 ? 1 : num_threads * 4;

        /* [0..num_chunks] first_word[c] = index of the first word of chunk c; first_word[num_chunks] = num_words
           [0..num_chunks] ones_before[c] = number of ones in the chunks before chunk c (after the prefix sums) */
        std::vector<uint64_t> first_word(num_chunks + 1), ones_before(num_chunks + 1, 0);
        for (uint64_t chunk = 0; chunk <= num_chunks; chunk++) first_word[chunk] = (chunk * num_words) / num_chunks;

        #pragma omp parallel for num_threads(num_threads)
        for (uint64_t chunk = 0; chunk < num_chunks; chunk++) {
            uint64_t count = 0;

            for (uint64_t i = first_word[chunk]; i < first_word[chunk + 1]; i++) {
                count += __builtin_popcountll(word_without_tail(words, i, bits.size()));
            }

            ones_before[chunk + 1] = count;
        }

        for (uint64_t chunk = 0; chunk < num_chunks; chunk++) ones_before[chunk + 1] += ones_before[chunk];
        _num_ones = ones_before[num_chunks];

        // write the positions of the ones of each chunk
        no_init_vector<uint64_t> positions;
        resize_no_init(positions, _num_ones);

        #pragma omp parallel for num_threads(num_threads)
        for (uint64_t chunk = 0; chunk < num_chunks; chunk++) {
            uint64_t idx = ones_before[chunk];

            for (uint64_t i = first_word[chunk]; i < first_word[chunk + 1]; i++) {
                uint64_t word = word_without_tail(words, i, bits.size());

                while (word != 0) {
                    positions[idx++] = i * 64 + __builtin_ctzll(word);
                    word &= word - 1;
                }
            }
        }

        if (_num_ones > 0) {
            _ef = sux::bits::EliasFano<>([&](uint64_t i) { return positions[i]; }, _num_ones, _size, num_threads);
            advise_huge_pages_for_arrays();
        }
    }

    // ############################# MISC METHODS #############################

    // returns the length of the bit vector
    inline uint64_t size() const { return _size; }

    // returns whether the bit vector contains no ones
    inline bool empty() const { return _num_ones == 0; }

    // returns the number of ones
    inline uint64_t num_ones() const { return _num_ones; }

    // returns the number of zeros
    inline uint64_t num_zeros() const { return _size - _num_ones; }

    // returns the size of the data structure in bytes
    uint64_t size_in_bytes() const { return sizeof(*this) + (_num_ones == 0 ? 0 : _ef.bitCount() / 8); }

    // ############################# ACCESS METHODS #############################

    // returns whether there is a one at index i (i in [0..size()-1])
    inline bool operator[](uint64_t i) const { return rank_1(i + 1) != rank_1(i); }

    // ############################# QUERY METHODS #############################

    // returns the number of ones before index i (i in [0..size()])
    inline uint64_t rank_1(uint64_t i) const { return _num_ones == 0 ? 0 : _ef.rank(i); }

    // returns the number of zeros before index i (i in [0..size()])
    inline uint64_t rank_0(uint64_t i) const { return i - rank_1(i); }

    // returns the index of the i-th one (i in [1..num_ones()])
    inline uint64_t select_1(uint64_t i) const { return _ef.select(i - 1); }

    /**
     * @brief returns the indices of the i-th and the (i+1)-th one, using a single select query
     * @param i [1..num_ones()-1]
     * @return (index of the i-th one, index of the (i+1)-th one)
     */
    inline std::pair<uint64_t, uint64_t> select_1_pair(uint64_t i) const
    {
        uint64_t next = 0;
        // this overload of select is not const, but it does not modify _ef
        const uint64_t pos = const_cast<sux::bits::EliasFano<>&>(_ef).select(i - 1, &next);
        return { pos, next };
    }

    /**
     * @brief returns the index of the i-th zero with a binary search over rank_0 in O(log(size())) time
     * @param i [1..num_zeros()] (so the bit vector must not be empty)
     */
    uint64_t select_0(uint64_t i) const
    {
        uint64_t lo = 0;
        uint64_t hi = _size - 1;

        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (rank_0(mid + 1) < i) lo = mid + 1; else hi = mid;
        }

        return lo;
    }

    // ############################# SERIALIZATION METHODS #############################

    /**
     * @brief writes the bit vector to an output stream
     * @param out output stream
     */
    void serialize(std::ostream& out) const
    {
        out.write((char*) &_size, sizeof(uint64_t));
        out.write((char*) &_num_ones, sizeof(uint64_t));
        if (_num_ones > 0) _ef.serialize(out);
    }

    /**
     * @brief reads a serialized bit vector from an input stream
     * @param in input stream
     * @param num_threads number of threads to use for rebuilding the select data structures
     */
    void load(std::istream& in, uint64_t num_threads = 1)
    {
        in.read((char*) &_size, sizeof(uint64_t));
        in.read((char*) &_num_ones, sizeof(uint64_t));
        if (_num_ones > 0) { _ef.load(in, num_threads); advise_huge_pages_for_arrays(); }
    }

    std::ostream& operator>>(std::ostream& os) const { serialize(os); return os; }
    std::istream& operator<<(std::istream& is) { load(is); return is; }
};

#endif // STRING_UTILS_DATA_STRUCTURES_ELIAS_FANO_BIT_VECTOR_HPP
