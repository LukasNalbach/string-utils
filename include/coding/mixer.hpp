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
 * @file mixer.hpp
 * @brief logistic mixing of the predictions of several bit models, and values coded bit by bit along a binary tree
 */

#ifndef STRING_UTILS_CODING_MIXER_HPP
#define STRING_UTILS_CODING_MIXER_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <coding/range_coder.hpp>
#include <misc/packed_integers.hpp>

/* a probability is stretched to its log-odds ln(p / (1 - p)), stored in units of 2^-stretch_fraction_bits in
   [-stretch_limit..stretch_limit] */
inline constexpr uint32_t stretch_fraction_bits = 8;

// number of bits of a quantized probability: the stretch table has an entry for each quantized probability
inline constexpr uint32_t quantized_probability_bits = 12;

// the largest absolute value of a stretched probability; the stretch values fill the 12-bit signed integers
inline constexpr int32_t stretch_limit = (int32_t(1) << (quantized_probability_bits - 1)) - 1;

// a weight of the mixer is stored in units of 2^-weight_fraction_bits
inline constexpr uint32_t weight_fraction_bits = probability_bits;

// the learning rate of the mixer is 2^-mixer_learning_rate_bits
inline constexpr uint32_t mixer_learning_rate_bits = 8;

// number of fraction bits of the fixed-point values of exp_table
inline constexpr uint32_t exp_fraction_bits = std::numeric_limits<uint64_t>::digits - 2;

// [0..stretch_limit] exp_table[x] = e^(-x / 2^stretch_fraction_bits) in units of 2^-exp_fraction_bits (computed
// with integers only, so that the encoder and the decoder compute the same tables on every machine)
inline const std::array<uint64_t, stretch_limit + 1> exp_table = [] {
    const uint64_t one = uint64_t(1) << exp_fraction_bits;
    // e^(-2^-stretch_fraction_bits) by its Taylor series
    uint64_t term = one;
    uint64_t step = one;

    for (uint64_t k = 1; term != 0; k++) {
        term /= (uint64_t(1) << stretch_fraction_bits) * k;
        step = k % 2 == 1 ? step - term : step + term;
    }

    std::array<uint64_t, stretch_limit + 1> table {};
    table[0] = one;

    for (uint64_t x = 1; x < table.size(); x++) {
        table[x] = uint64_t((uint128_t(table[x - 1]) * step) >> exp_fraction_bits);
    }

    return table;
}();

// [0..2*stretch_limit] squash_table[x + stretch_limit] = 1 / (1 + e^(-x / 2^stretch_fraction_bits)) in units of
// 2^-probability_bits, in [1..2^16-1]
inline const std::array<uint16_t, 2 * stretch_limit + 1> squash_table = [] {
    const uint64_t one = uint64_t(1) << exp_fraction_bits;
    std::array<uint16_t, 2 * stretch_limit + 1> table {};

    for (int32_t x = 0; x <= stretch_limit; x++) {
        const uint128_t denominator = uint128_t(one) + exp_table[x];
        const uint64_t p = uint64_t((uint128_t(probability_one) * one + denominator / 2) / denominator);
        table[stretch_limit + x] = uint16_t(std::min<uint64_t>(p, probability_one - 1));
        table[stretch_limit - x] = uint16_t(probability_one - table[stretch_limit + x]);
    }

    return table;
}();

// [0..2^12-1] stretch_table[q] = the smallest x with squash(x) >= the center of the q-th quantized probability (the
// inverse of squash)
inline const std::array<int16_t, uint64_t(1) << quantized_probability_bits> stretch_table = [] {
    constexpr uint32_t quantum = probability_one >> quantized_probability_bits;
    std::array<int16_t, uint64_t(1) << quantized_probability_bits> table {};
    int32_t x = -stretch_limit;

    for (uint32_t q = 0; q < table.size(); q++) {
        while (x < stretch_limit && squash_table[x + stretch_limit] < q * quantum + quantum / 2) x++;
        table[q] = int16_t(x);
    }

    return table;
}();

// returns the stretched probability of a 1-bit of a model
inline int32_t stretch(const bit_model& model)
{
    return stretch_table[(probability_one - model.p0) >> (probability_bits - quantized_probability_bits)];
}

// returns the probability of a 1-bit (in units of 2^-probability_bits) of a stretched probability x
inline uint32_t squash(int64_t x)
{
    return squash_table[std::clamp<int64_t>(x, -stretch_limit, stretch_limit) + stretch_limit];
}

/**
 * @brief k context models of the bits of values of a fixed number of bits (depth), each a binary tree of bit models
 *        per context, whose predictions are mixed with one weight vector per tree node
 * @tparam k the number of models
 */
template <uint64_t k>
class mixed_trees {
private:
    uint8_t depth = 0; // the number of bits of a value
    // [0..k-1] the bit models of model i: [ctx * 2^depth + node] the model of a tree node (1..2^depth-1) in context ctx
    std::array<std::vector<bit_model>, k> models;
    std::vector<int32_t> weights; // [node * k + i] the weight of model i at a tree node

public:
    mixed_trees() = default;

    /**
     * @brief creates the models
     * @param depth the number of bits of a value
     * @param num_contexts [0..k-1] the number of contexts of each model
     */
    mixed_trees(uint8_t depth, const std::array<uint64_t, k>& num_contexts) : depth(depth)
    {
        for (uint64_t i = 0; i < k; i++) models[i].resize(num_contexts[i] << depth);
        weights.assign(k << depth, int32_t((uint32_t(1) << weight_fraction_bits) / k));
    }

    /**
     * @brief codes a value, the highest bit first, and updates the models
     * @param coder a range_encoder or a range_decoder
     * @param value the value (ignored by a range_decoder)
     * @param contexts [0..k-1] the context of each model
     * @return the value
     */
    template <typename coder_t>
    uint64_t code(coder_t& coder, uint64_t value, const std::array<uint64_t, k>& contexts)
    {
        uint64_t node = 1;

        for (uint8_t level = depth; level > 0; level--) {
            std::array<bit_model*, k> m;
            std::array<int32_t, k> st;
            int32_t* w = weights.data() + node * k;
            int64_t dot = 0;

            for (uint64_t i = 0; i < k; i++) {
                m[i] = &models[i][(contexts[i] << depth) + node];
                st[i] = stretch(*m[i]);
                dot += int64_t(w[i]) * st[i];
            }

            const uint32_t p1 = squash(dot >> weight_fraction_bits);
            const bool bit = coder.code((value >> (level - 1)) & 1, probability_one - p1);
            const int64_t error = (int64_t(bit) << probability_bits) - int64_t(p1);

            for (uint64_t i = 0; i < k; i++) {
                w[i] += int32_t((error * st[i]) >> (stretch_fraction_bits + mixer_learning_rate_bits));
                m[i]->update(bit);
            }

            node = 2 * node + bit;
        }

        return node - (uint64_t(1) << depth);
    }
};

/**
 * @brief codes values bit by bit along a binary tree of bit models per context
 */
class bit_trees {
private:
    uint8_t depth = 0; // the largest number of bits of a value
    std::vector<bit_model> models; // [ctx * 2^depth + node] the model of the tree node in context ctx

public:
    bit_trees() = default;

    /**
     * @brief creates the models
     * @param depth the largest number of bits of a value
     * @param num_contexts the number of contexts
     */
    bit_trees(uint8_t depth, uint64_t num_contexts) : depth(depth), models(num_contexts << depth) { }

    /**
     * @brief codes a value of num_bits <= depth bits, the highest bit first, and updates the models
     * @param coder a range_encoder or a range_decoder
     * @param value the value (ignored by a range_decoder)
     * @param num_bits its number of bits (the same in each call with a context)
     * @param context the context
     * @return the value
     */
    template <typename coder_t>
    uint64_t code(coder_t& coder, uint64_t value, uint8_t num_bits, uint64_t context)
    {
        bit_model* tree = models.data() + (context << depth);
        uint64_t node = 1;

        for (uint8_t level = num_bits; level > 0; level--) {
            const bool bit = coder.code((value >> (level - 1)) & 1, tree[node].p0);
            tree[node].update(bit);
            node = 2 * node + bit;
        }

        return node - (uint64_t(1) << num_bits);
    }
};

#endif // STRING_UTILS_CODING_MIXER_HPP
