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

#include <misc/repetitive_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

#endif // STRING_UTILS_TESTS_TEST_INPUTS_HPP
