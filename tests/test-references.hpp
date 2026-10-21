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

#ifndef STRING_UTILS_TESTS_TEST_REFERENCES_HPP
#define STRING_UTILS_TESTS_TEST_REFERENCES_HPP

// reference implementations for the tests: suffix array construction with libsais, the LCP array, and range minimum
// queries on an integer array

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <omp.h>
#include <gtl/phmap.hpp>
#include <libsais.h>

#include <misc/sort.hpp>
#include <misc/utils.hpp>

/**
 * @brief returns the suffix array of input with an appended sentinel 0 ([0..n], SA[0] = n for n = input_size);
 *        one-byte symbols are ordered by their unsigned value, wider symbols by their unsigned bit pattern
 * @tparam sym_t symbol type of the input (a one-byte type for a byte alphabet, or a wider integer alphabet)
 * @tparam inp_t sequence type of the input (e.g. std::string or std::vector<int32_t>)
 * @param input the input sequence (taken by value, as it is modified internally)
 * @param input_size the length of the input
 * @param num_threads the number of threads to use for the suffix array construction
 * @return the suffix array of input + the sentinel (of size input_size + 1)
 */
template <typename sym_t, typename inp_t>
inline std::vector<int64_t> compute_reference_sa(inp_t input, uint64_t input_size, uint64_t num_threads)
{
    // the suffix array is built with the 32-bit libsais and copied to 64-bit values afterwards
    if (input_size + 1 > uint64_t(std::numeric_limits<int32_t>::max()))
        throw std::runtime_error("the reference suffix array is built with the 32-bit libsais");

    no_init_vector<int32_t> sa32;
    resize_no_init(sa32, input_size + 1);

    if constexpr (sizeof(sym_t) == 1) {
        // [0..255] contains_uchar[c] = 1 <=> the byte c occurs in input
        uint8_t contains_uchar[256] = {};

        #pragma omp parallel for num_threads(num_threads) reduction(| : contains_uchar[:256])
        for (uint64_t i = 0; i < input_size; i++)
            contains_uchar[symbol_bit_pattern(input[i])] = 1;

        /* if the input contains a 0-byte, remap the occurring bytes to 1, 2, ... in increasing order (this does
           not change the suffix array), so that only the sentinel is 0 */
        if (contains_uchar[0]) {
            // [0..255] map_uchar[c] = new value of the byte c (if c occurs in input)
            std::vector<uint8_t> map_uchar(256, 0);
            uint8_t next_uchar = 1;

            for (uint16_t i = 0; i < 256; i++)
                if (contains_uchar[i]) map_uchar[i] = next_uchar++;

            #pragma omp parallel for num_threads(num_threads)
            for (uint64_t i = 0; i < input_size; i++)
                input[i] = static_cast<sym_t>(map_uchar[symbol_bit_pattern(input[i])]);
        }

        input.push_back(sym_t(0));
        int32_t error = libsais_omp(
            (uint8_t*) &input[0], &sa32[0], int32_t(input_size + 1), 0, NULL, int32_t(num_threads));
        if (error != 0) throw std::runtime_error("libsais_omp failed with " + std::to_string(error));
    } else {
        // remap the symbols to their ranks [1..sigma] in the alphabet, ordered by their unsigned bit pattern
        using usym_t = std::make_unsigned_t<sym_t>;

        // the alphabet: [0..sigma-1] the distinct symbols of input as unsigned bit patterns, in increasing order
        no_init_vector<usym_t> alphabet;
        resize_no_init(alphabet, input_size);
        for (uint64_t i = 0; i < input_size; i++) alphabet[i] = usym_t(symbol_bit_pattern(sym_t(input[i])));
        sort_parallel(alphabet.begin(), alphabet.end(), std::less<>(), num_threads);
        alphabet.resize(std::unique(alphabet.begin(), alphabet.end()) - alphabet.begin());

        // rank_of_symbol[alphabet[x]] = x + 1
        gtl::flat_hash_map<usym_t, int64_t> rank_of_symbol;
        for (uint64_t x = 0; x < alphabet.size(); x++) rank_of_symbol[alphabet[x]] = int64_t(x + 1);

        const gtl::flat_hash_map<usym_t, int64_t>& ranks = rank_of_symbol;
        no_init_vector<int32_t> input_libsais;
        resize_no_init(input_libsais, input_size + 1);

        #pragma omp parallel for num_threads(num_threads)
        for (uint64_t i = 0; i < input_size; i++)
            input_libsais[i] = int32_t(ranks.at(usym_t(symbol_bit_pattern(sym_t(input[i])))));

        input_libsais[input_size] = 0;
        int32_t error = libsais_int_omp(&input_libsais[0], &sa32[0],
            int32_t(input_size + 1), int32_t(alphabet.size() + 1), 0, int32_t(num_threads));
        if (error != 0) throw std::runtime_error("libsais_int_omp failed with " + std::to_string(error));
    }

    std::vector<int64_t> suffix_array(sa32.begin(), sa32.end());

    // check that each suffix array value is in [0, input_size]
    for (uint64_t i = 0; i <= input_size; i++) {
        if (suffix_array[i] < 0 || suffix_array[i] > (int64_t) input_size) {
            throw std::runtime_error("the reference suffix array contains " + std::to_string(suffix_array[i])
                + " at position " + std::to_string(i) + ", outside [0, " + std::to_string(input_size) + "]");
        }
    }

    return suffix_array;
}

/**
 * @brief computes the LCP array of a text from its suffix array (Kasai et al.)
 * @tparam text_t sequence type of the text
 * @tparam sa_t sequence type of the suffix array
 * @param text the text T = text[0..|text|-1]
 * @param SA [0..|SA|-1] the suffix array of T (|SA| = |text|) or of T with a sentinel (|SA| = |text| + 1)
 * @return [0..|SA|-1] LCP[0] = 0 and LCP[i] is the length of the longest common prefix of T[SA[i-1]..] and
 *         T[SA[i]..]
 */
template <typename text_t, typename sa_t>
inline std::vector<uint64_t> compute_reference_lcp(const text_t& text, const sa_t& SA)
{
    const uint64_t n = text.size();
    const uint64_t num_suffixes = SA.size();
    std::vector<uint64_t> LCP(num_suffixes, 0);

    // [0..|SA|-1] ISA[SA[i]] = i
    std::vector<uint64_t> ISA(num_suffixes);
    for (uint64_t i = 0; i < num_suffixes; i++) ISA[uint64_t(SA[i])] = i;

    // LCP[ISA[i+1]] >= LCP[ISA[i]] - 1, so l decreases by at most one per suffix
    uint64_t l = 0;

    for (uint64_t i = 0; i < num_suffixes; i++) {
        if (ISA[i] == 0) {
            l = 0;
            continue;
        }

        const uint64_t j = uint64_t(SA[ISA[i] - 1]);
        while (i + l < n && j + l < n && text[i + l] == text[j + l]) l++;
        LCP[ISA[i]] = l;
        if (l > 0) l--;
    }

    return LCP;
}

// returns the minimum of values[lo..hi] (lo <= hi)
template <typename value_t>
inline value_t reference_range_min(const std::vector<value_t>& values, uint64_t lo, uint64_t hi)
{
    return *std::min_element(values.begin() + lo, values.begin() + hi + 1);
}

/**
 * @brief returns the positions i in [lo..hi] with values[i] = value, in increasing order
 * @tparam pos_t type of a position
 */
template <typename pos_t = uint64_t, typename value_t>
inline std::vector<pos_t> reference_positions_of(
    const std::vector<value_t>& values, uint64_t lo, uint64_t hi, value_t value)
{
    std::vector<pos_t> positions;
    for (uint64_t i = lo; i <= hi; i++) if (values[i] == value) positions.emplace_back(pos_t(i));
    return positions;
}

/**
 * @brief the reference of the query for_each_min_pos_with_min_between: let p_0 < p_1 < ... be the positions i in
 *        [lo..hi] with values[i] = value, and p_{-1} = lo - 1
 * @return the pairs (p_k, minimum of values[p_{k-1}+1..p_k-1]) in increasing order of p_k, and the minimum of the
 *         values after the last position p_k up to hi; the minimum of no values is std::numeric_limits<int64_t>::max()
 */
inline std::pair<std::vector<std::pair<uint64_t, int64_t>>, int64_t> reference_min_between(
    const std::vector<int64_t>& values, uint64_t lo, uint64_t hi, int64_t value)
{
    std::vector<std::pair<uint64_t, int64_t>> min_between;
    int64_t running = std::numeric_limits<int64_t>::max(); // the minimum since the last position of value

    for (uint64_t i = lo; i <= hi; i++) {
        if (values[i] == value) {
            min_between.emplace_back(i, running);
            running = std::numeric_limits<int64_t>::max();
        } else {
            running = std::min(running, values[i]);
        }
    }

    return { std::move(min_between), running };
}

#endif // STRING_UTILS_TESTS_TEST_REFERENCES_HPP
