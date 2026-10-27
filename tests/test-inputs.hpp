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

#ifndef STRING_UTILS_TESTS_TEST_INPUTS_HPP
#define STRING_UTILS_TESTS_TEST_INPUTS_HPP

#include <coding/byte_codes.hpp>
#include <misc/repetitive_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

/**
 * @brief draws an integer in [min_size, max_size] whose logarithm is uniformly distributed
 * @tparam gen_t random number generator type
 * @param min_size minimum size (inclusive)
 * @param max_size maximum size (inclusive)
 * @param gen random number generator
 * @return a log-uniformly distributed size in [min_size, max_size]
 */
template <typename gen_t>
inline uint64_t random_log_uniform_size(uint64_t min_size, uint64_t max_size, gen_t& gen)
{
    std::uniform_real_distribution<double> log_distrib(
        std::log((double) std::max<uint64_t>(1, min_size)),
        std::log((double) std::max<uint64_t>(1, max_size)));
    return std::clamp<uint64_t>((uint64_t) std::llround(std::exp(log_distrib(gen))), min_size, max_size);
}

/**
 * @brief generates a random repetitive sequence of a random kind (see repetitive_input_kind_t)
 * @tparam inp_t sequence type to generate (e.g. std::string or std::vector<int32_t>)
 * @param min_size minimum length of the generated sequence
 * @param max_size maximum length of the generated sequence
 * @param min_sym smallest symbol value that may occur in the sequence
 * @param max_sym largest symbol value that may occur in the sequence
 * @param seed seed of the random number generator (0: a random seed is used)
 * @return a random sequence with a length in [min_size, max_size]
 */
template <typename inp_t>
inline inp_t random_repetitive_input(
    uint64_t min_size, uint64_t max_size,
    typename inp_t::value_type min_sym = std::numeric_limits<typename inp_t::value_type>::min(),
    typename inp_t::value_type max_sym = std::numeric_limits<typename inp_t::value_type>::max(),
    uint64_t seed = 0)
{
    std::random_device device;
    std::mt19937_64 random(seed == 0 ? device() : seed);
    uint64_t size = random_log_uniform_size(min_size, max_size, random);

    return generate_repetitive_input<inp_t>(
        random_repetitive_params(size, random), min_sym, max_sym);
}

/**
 * @brief draws the alphabet of a random byte text: up to max_small_sigma bytes, up to 256 bytes or all 256 bytes (each
 *        with probability 1/3), drawn from all byte values
 * @tparam gen_t random number generator type
 * @param max_small_sigma the largest size of a small alphabet
 * @param gen random number generator
 * @return the distinct bytes of the alphabet in random order
 */
template <typename gen_t>
inline std::vector<uint8_t> random_byte_alphabet(uint64_t max_small_sigma, gen_t& gen)
{
    std::vector<uint8_t> bytes(num_byte_values);
    std::iota(bytes.begin(), bytes.end(), uint8_t(0));
    std::shuffle(bytes.begin(), bytes.end(), gen);
    const uint64_t kind = std::uniform_int_distribution<uint64_t>(0, 2)(gen); // 0: small, 1: any size, 2: all bytes
    const uint64_t max_sigma = kind == 0 ? std::min(max_small_sigma, num_byte_values) : num_byte_values;
    bytes.resize(kind == 2 ? num_byte_values : std::uniform_int_distribution<uint64_t>(1, max_sigma)(gen));
    return bytes;
}

#endif // STRING_UTILS_TESTS_TEST_INPUTS_HPP
