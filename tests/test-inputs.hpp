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

/**
 * @file test-inputs.hpp
 * @brief random inputs for the tests: sizes, alphabets and texts
 */

#ifndef STRING_UTILS_TESTS_TEST_INPUTS_HPP
#define STRING_UTILS_TESTS_TEST_INPUTS_HPP

#include <coding/byte_codes.hpp>
#include <fasta/alphabet.hpp>
#include <misc/repetitive_input.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <type_traits>
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
 * @brief draws the alphabet of a random byte text: up to max_small_sigma bytes, up to 256 bytes, all 256 bytes, or the
 *        bytes of fasta_default_alphabet and up to max_small_sigma bytes in total (each with probability 1/4), drawn from
 *        all byte values
 * @tparam gen_t random number generator type
 * @param max_small_sigma the largest size of a small alphabet
 * @param gen random number generator
 * @return the distinct bytes of the alphabet in random order
 */
template <typename gen_t>
inline std::vector<uint8_t> random_byte_alphabet(uint64_t max_small_sigma, gen_t& gen)
{
    enum kind_t : uint64_t { small, any_size, all_bytes, dna, num_kinds };
    std::vector<uint8_t> bytes(num_byte_values);
    std::iota(bytes.begin(), bytes.end(), uint8_t(0));
    std::shuffle(bytes.begin(), bytes.end(), gen);
    const kind_t kind = kind_t(std::uniform_int_distribution<uint64_t>(0, num_kinds - 1)(gen));
    const uint64_t max_sigma = kind == small || kind == dna ? std::min(max_small_sigma, num_byte_values) : num_byte_values;
    if (kind == all_bytes) return bytes;

    if (kind == dna) {
        // the bases first, then the other bytes
        std::stable_partition(bytes.begin(), bytes.end(),
            [](uint8_t c) { return fasta_default_alphabet.find(char(c)) != std::string_view::npos; });
        const uint64_t num_bases = fasta_default_alphabet.size();
        bytes.resize(std::uniform_int_distribution<uint64_t>(num_bases, std::max(num_bases, max_sigma))(gen));
        std::shuffle(bytes.begin(), bytes.end(), gen);
        return bytes;
    }

    bytes.resize(std::uniform_int_distribution<uint64_t>(1, max_sigma)(gen));
    return bytes;
}

/**
 * @brief an alphabet of integer symbols: sigma symbols in increasing order with the same difference step, the smallest
 *        one first values above the smallest value of sym_t
 * @tparam sym_t the symbol type
 */
template <typename sym_t>
struct integer_alphabet {
    using usym_t = std::make_unsigned_t<sym_t>;

    uint64_t sigma = 1; // the number of symbols
    uint64_t first = 0; // the difference of the smallest symbol and the smallest value of sym_t
    uint64_t step = 1; // the difference of two consecutive symbols

    // returns the symbol of rank x in [0, sigma)
    sym_t operator[](uint64_t x) const
    {
        return sym_t(usym_t(usym_t(std::numeric_limits<sym_t>::min()) + usym_t(first + x * step)));
    }
};

/**
 * @brief draws an alphabet of integer symbols: sigma log-uniform in [min_sigma, max_sigma], the step 1 or (with
 *        probability 1/2) log-uniform up to the largest step with which the symbols fit, and the smallest symbol at the
 *        smallest value of sym_t, at 1, as large as possible or random (each with probability 1/4)
 * @tparam sym_t the symbol type
 * @tparam gen_t random number generator type
 * @param min_sigma the smallest number of symbols
 * @param max_sigma the largest number of symbols
 * @param gen random number generator
 * @param max_value_difference the largest difference of a symbol and the smallest value of sym_t (at most the largest
 *        difference of two values of sym_t)
 */
template <typename sym_t, typename gen_t>
inline integer_alphabet<sym_t> random_integer_alphabet(uint64_t min_sigma, uint64_t max_sigma, gen_t& gen,
    uint64_t max_value_difference = std::numeric_limits<std::make_unsigned_t<sym_t>>::max())
{
    enum placement_t : uint64_t { at_smallest_value, at_one, at_largest_value, random_place, num_placements };
    using usym_t = std::make_unsigned_t<sym_t>;
    const uint64_t max_difference = std::min<uint64_t>(max_value_difference, std::numeric_limits<usym_t>::max());
    integer_alphabet<sym_t> alphabet;
    // the number of values that fit, at most 2^64 - 1
    const uint64_t num_values = max_difference + (max_difference < std::numeric_limits<uint64_t>::max() ? 1 : 0);
    alphabet.sigma = random_log_uniform_size(std::min(min_sigma, num_values), std::min(max_sigma, num_values), gen);
    const uint64_t max_step = alphabet.sigma == 1 ? 1 : max_difference / (alphabet.sigma - 1);
    alphabet.step = std::bernoulli_distribution()(gen) ? 1 : random_log_uniform_size(1, max_step, gen);
    const uint64_t max_first = max_difference - (alphabet.sigma - 1) * alphabet.step;
    // the difference of the value 1 and the smallest value of sym_t
    const uint64_t first_at_one = uint64_t(usym_t(usym_t(1) - usym_t(std::numeric_limits<sym_t>::min())));

    switch (placement_t(std::uniform_int_distribution<uint64_t>(0, num_placements - 1)(gen))) {
        case at_smallest_value: alphabet.first = 0; break;
        case at_one: alphabet.first = std::min(first_at_one, max_first); break;
        case at_largest_value: alphabet.first = max_first; break;
        default: alphabet.first = std::uniform_int_distribution<uint64_t>(0, max_first)(gen); break;
    }

    return alphabet;
}

/**
 * @brief the structures of a random text (see random_text_ranks)
 */
enum text_structure_t : uint64_t {
    // a repetitive sequence of a random kind (see random_repetitive_params)
    _text_repetitive,
    // pieces over disjoint ranges of the symbols, each a repetitive sequence
    _text_segments,
    // a repetitive sequence over all but a few symbols, each of which replaces a few random positions
    _text_rare_symbols,
    // runs of the symbol 0 of the lengths 1, 2, 3, ..., each after a random other symbol
    _text_growing_runs,
    // groups b 0 c d: b is 1 or 2, d is 3 and c is the next of the symbols from 4 on (cyclically)
    _text_contexts,
    // the number of structures
    _num_text_structures
};

/**
 * @brief returns a random text of length n over the ranks [0, sigma) with a random structure (each with the same
 *        probability, see text_structure_t; a structure that needs more symbols than sigma is replaced by
 *        _text_repetitive); if n >= sigma, every rank occurs in the text
 * @tparam gen_t random number generator type
 * @param n the length of the text
 * @param sigma the number of ranks (sigma >= 1)
 * @param gen random number generator
 * @return [0..n-1] the ranks of the symbols of the text
 */
template <typename gen_t>
inline std::vector<uint64_t> random_text_ranks(uint64_t n, uint64_t sigma, gen_t& gen)
{
    auto uniform = [&](uint64_t min, uint64_t max) { return std::uniform_int_distribution<uint64_t>(min, max)(gen); };

    // returns a repetitive sequence of length len over the ranks [first, first + num)
    auto repetitive = [&](uint64_t len, uint64_t first, uint64_t num) {
        return generate_repetitive_input<std::vector<uint64_t>>(random_repetitive_params(len, gen), first,
            first + num - 1);
    };

    // the symbols of a group of _text_contexts: the symbol 0, the two preceding symbols, the last symbol, the first
    // following symbol
    constexpr uint64_t context_symbol = 0;
    constexpr uint64_t first_preceding_symbol = context_symbol + 1;
    constexpr uint64_t num_preceding_symbols = 2;
    constexpr uint64_t group_end_symbol = first_preceding_symbol + num_preceding_symbols;
    constexpr uint64_t first_following_symbol = group_end_symbol + 1;

    std::vector<uint64_t> text;
    if (n == 0) return text;
    const text_structure_t structure = text_structure_t(uniform(0, _num_text_structures - 1));
    // a few: up to the number of bits of n
    const uint64_t max_few = std::bit_width(n);

    if (structure == _text_segments && sigma > 1) {
        // the parts of the symbols: [0..num_parts] the parts are [cuts[j], cuts[j + 1])
        std::vector<uint64_t> cuts { 0, sigma };
        for (uint64_t j = uniform(1, std::min(sigma - 1, max_few)); j > 0; j--) cuts.push_back(uniform(1, sigma - 1));
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        while (text.size() < n) {
            const uint64_t part = uniform(0, cuts.size() - 2);
            const std::vector<uint64_t> piece = repetitive(random_log_uniform_size(1, n - text.size(), gen),
                cuts[part], cuts[part + 1] - cuts[part]);
            text.insert(text.end(), piece.begin(), piece.end());
        }
    } else if (structure == _text_rare_symbols && sigma > 1) {
        // the rare symbols are the largest ranks
        const uint64_t num_rare = uniform(1, std::min(sigma - 1, max_few));
        text = repetitive(n, 0, sigma - num_rare);

        for (uint64_t x = sigma - num_rare; x < sigma; x++) {
            for (uint64_t k = uniform(1, max_few); k > 0; k--) text[uniform(0, n - 1)] = x;
        }
    } else if (structure == _text_growing_runs && sigma > 1) {
        for (uint64_t length = 1; text.size() < n; length++) {
            text.push_back(uniform(1, sigma - 1));
            text.insert(text.end(), std::min(length, n - text.size()), context_symbol);
        }
    } else if (structure == _text_contexts && sigma > first_following_symbol) {
        for (uint64_t group = 0; text.size() < n; group++) {
            const uint64_t following = first_following_symbol + group % (sigma - first_following_symbol);
            for (uint64_t x : { first_preceding_symbol + uniform(0, num_preceding_symbols - 1), context_symbol,
                following, group_end_symbol }) if (text.size() < n) text.push_back(x);
        }
    } else {
        text = repetitive(n, 0, sigma);
    }

    // the ranks that do not occur replace the symbols at evenly spaced positions
    if (n >= sigma) {
        std::vector<bool> occurs(sigma, false);
        for (uint64_t x : text) occurs[x] = true;
        std::vector<uint64_t> missing;
        for (uint64_t x = 0; x < sigma; x++) if (!occurs[x]) missing.push_back(x);
        for (uint64_t j = 0; j < missing.size(); j++) text[j * n / missing.size()] = missing[j];
    }

    return text;
}

/**
 * @brief returns a random text with a length in [min_size, max_size] (log-uniform, see random_log_uniform_size) and a
 *        random structure (see random_text_ranks) over an alphabet of sigma symbols
 * @tparam inp_t the container of the text (e.g. std::string or std::vector<int32_t>)
 * @tparam alphabet_t type of alphabet
 * @tparam gen_t random number generator type
 * @param min_size the smallest length
 * @param max_size the largest length
 * @param sigma the number of symbols of the alphabet
 * @param alphabet alphabet[x] is the symbol of rank x in [0, sigma)
 * @param gen random number generator
 */
template <typename inp_t, typename alphabet_t, typename gen_t>
inline inp_t random_text(uint64_t min_size, uint64_t max_size, uint64_t sigma, const alphabet_t& alphabet, gen_t& gen)
{
    const std::vector<uint64_t> ranks = random_text_ranks(random_log_uniform_size(min_size, max_size, gen), sigma, gen);
    inp_t text(ranks.size(), typename inp_t::value_type());
    for (uint64_t i = 0; i < ranks.size(); i++) text[i] = typename inp_t::value_type(alphabet[ranks[i]]);
    return text;
}

#endif // STRING_UTILS_TESTS_TEST_INPUTS_HPP
