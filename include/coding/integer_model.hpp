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
 * @file integer_model.hpp
 * @brief adaptive models of unsigned integers and of single decisions, coded with the range coder
 */

#ifndef STRING_UTILS_CODING_INTEGER_MODEL_HPP
#define STRING_UTILS_CODING_INTEGER_MODEL_HPP

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/mixer.hpp>
#include <coding/range_coder.hpp>
#include <misc/bits.hpp>

// number of bits below the leading one of an integer that are coded with bit models (the others with probability 1/2)
inline constexpr uint8_t modeled_length_bits = 8;

// the largest bit width of a 64-bit integer
inline constexpr uint64_t max_integer_width = std::numeric_limits<uint64_t>::digits;

/**
 * @brief codes integers in [0..2^64-1] in a context: the bit width b with a binary tree of bit models per context,
 *        then the modeled_length_bits bits below the leading one with a tree per (context, b), then the remaining
 *        bits with probability 1/2
 */
class integer_model {
private:
    bit_trees widths; // [ctx] the models of the bit widths
    bit_trees top_bits; // [ctx * (max_integer_width + 1) + b] the models of the bits below the leading one

public:
    integer_model() = default;

    /**
     * @brief creates the models
     * @param num_contexts the number of contexts
     */
    explicit integer_model(uint64_t num_contexts)
        : widths(std::bit_width(max_integer_width), num_contexts),
          top_bits(modeled_length_bits, num_contexts * (max_integer_width + 1)) { }

    /**
     * @brief codes a value and updates the models
     * @param coder a range_encoder or a range_decoder
     * @param value the value (ignored by a range_decoder)
     * @param context the context, less than the number of contexts
     * @return the value; a range_decoder throws std::runtime_error if the bit width is larger than 64
     */
    template <typename coder_t>
    uint64_t code(coder_t& coder, uint64_t value, uint64_t context)
    {
        const uint64_t width = widths.code(coder, std::bit_width(value), std::bit_width(max_integer_width), context);
        if (width > max_integer_width) throw invalid_file();
        if (width == 0) return 0;

        const uint8_t below = uint8_t(width - 1); // the number of bits below the leading one
        const uint8_t modeled = std::min(below, modeled_length_bits);
        const uint8_t raw = below - modeled;
        const uint64_t top = top_bits.code(coder, (value >> raw) & low_bits_mask(modeled), modeled,
            context * (max_integer_width + 1) + width);
        const uint64_t rest = coder.code_bits(value & low_bits_mask(raw), raw);
        return (uint64_t(1) << below) | (top << raw) | rest;
    }
};

/**
 * @brief codes yes/no decisions in a context with one bit model per context
 */
class decision_model {
private:
    std::vector<bit_model> models; // [ctx] the model of the context

public:
    decision_model() = default;

    /**
     * @brief creates the models
     * @param num_contexts the number of contexts
     */
    explicit decision_model(uint64_t num_contexts) : models(num_contexts) { }

    /**
     * @brief codes a decision and updates the model
     * @param coder a range_encoder or a range_decoder
     * @param yes the decision (ignored by a range_decoder)
     * @param context the context, less than the number of contexts
     * @return the decision
     */
    template <typename coder_t>
    bool code(coder_t& coder, bool yes, uint64_t context)
    {
        bit_model& model = models[context];
        const bool bit = coder.code(yes, model.p0);
        model.update(bit);
        return bit;
    }
};

#endif // STRING_UTILS_CODING_INTEGER_MODEL_HPP
