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
 * @file range_coder.hpp
 * @brief a binary arithmetic coder (range coder with 32-bit range and carry propagation) and adaptive bit models
 */

#ifndef STRING_UTILS_CODING_RANGE_CODER_HPP
#define STRING_UTILS_CODING_RANGE_CODER_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

// number of bits of a probability: a probability x / 2^probability_bits is stored as the integer x in [1..2^16-1]
inline constexpr uint32_t probability_bits = std::numeric_limits<uint16_t>::digits;

// the probability 1 (2^probability_bits)
inline constexpr uint32_t probability_one = uint32_t(1) << probability_bits;

// number of bits the coder outputs at once
inline constexpr uint32_t range_coder_output_bits = std::numeric_limits<uint8_t>::digits;

// the range is renormalized (shifted by range_coder_output_bits) while it is smaller than range_coder_min_range
inline constexpr uint32_t range_coder_min_range =
    uint32_t(1) << (std::numeric_limits<uint32_t>::digits - range_coder_output_bits);

// a bit model stops lowering its adaptation rate 1 / (count + 2) after this many updates (it counts them in a byte)
inline constexpr uint8_t bit_model_max_count = std::numeric_limits<uint8_t>::max();

// [0..bit_model_max_count] adaptation_rate[c] = 2^probability_bits / (c + 2), the rate of a model after c updates
inline constexpr std::array<uint32_t, bit_model_max_count + 1> adaptation_rate = [] {
    std::array<uint32_t, bit_model_max_count + 1> rate {};
    for (uint32_t c = 0; c <= bit_model_max_count; c++) rate[c] = probability_one / (c + 2);
    return rate;
}();

/**
 * @brief an adaptive model of a bit: the probability of a 0-bit, moved towards each coded bit by 1 / (c + 2) after c
 *        updates (the Krichevsky-Trofimov estimate until bit_model_max_count updates, then an exponential average)
 */
struct bit_model {
    uint16_t p0 = probability_one / 2; // probability of a 0-bit in units of 2^-probability_bits, in [1..2^16-1]
    uint8_t count = 0; // number of updates, at most bit_model_max_count

    // moves p0 towards the bit
    inline void update(bool bit)
    {
        const uint32_t rate = adaptation_rate[count];
        if (bit) p0 -= uint16_t((uint32_t(p0) * rate) >> probability_bits);
        else p0 += uint16_t(((probability_one - p0) * rate) >> probability_bits);
        count += count < bit_model_max_count;
    }
};

/**
 * @brief encodes bits with given probabilities into a byte sequence; the bytes after the last byte that is not 0 are
 *        not stored (the decoder reads 0-bytes after the end)
 */
class range_encoder {
private:
    std::vector<uint8_t> out; // the encoded bytes
    uint64_t low = 0; // the lower end of the interval (33 bits: bit 32 is a carry into the bytes not yet written)
    uint32_t range = std::numeric_limits<uint32_t>::max(); // the length of the interval
    uint8_t held = 0; // the byte that is held back until no carry can reach it
    uint64_t num_held = 1; // number of held back bytes: held followed by num_held - 1 bytes 0xFF
    bool first = true; // true <=> no byte has been written (the first held byte is 0 and is not stored)

    // moves the highest byte of the low 32 bits of low into the held bytes and writes the held bytes, if no carry
    // can reach them anymore
    inline void shift_low()
    {
        const uint8_t top_byte = uint8_t(low >> (std::numeric_limits<uint32_t>::digits - range_coder_output_bits));
        const uint8_t carry = uint8_t(low >> std::numeric_limits<uint32_t>::digits);

        if (top_byte != std::numeric_limits<uint8_t>::max() || carry != 0) {
            uint8_t byte = held;

            for (; num_held > 0; num_held--) {
                if (!first) out.push_back(uint8_t(byte + carry));
                first = false;
                byte = std::numeric_limits<uint8_t>::max();
            }

            held = top_byte;
        }

        num_held++;
        low = (low << range_coder_output_bits) & std::numeric_limits<uint32_t>::max();
    }

    // shifts the range while it is smaller than range_coder_min_range
    inline void normalize()
    {
        while (range < range_coder_min_range) {
            range <<= range_coder_output_bits;
            shift_low();
        }
    }

public:
    /**
     * @brief encodes a bit
     * @param bit the bit
     * @param p0 the probability of a 0-bit in units of 2^-probability_bits, in [1..2^16-1]
     */
    inline void encode(bool bit, uint32_t p0)
    {
        const uint32_t bound = (range >> probability_bits) * p0;

        if (bit) {
            low += bound;
            range -= bound;
        } else {
            range = bound;
        }

        normalize();
    }

    /**
     * @brief encodes a bit (the same interface as range_decoder::code)
     * @param bit the bit
     * @param p0 the probability of a 0-bit in units of 2^-probability_bits, in [1..2^16-1]
     * @return the bit
     */
    inline bool code(bool bit, uint32_t p0)
    {
        encode(bit, p0);
        return bit;
    }

    /**
     * @brief encodes the lowest bits of a value, each with probability 1/2, the highest one first
     * @param value the value
     * @param num_bits number of bits
     */
    inline void encode_bits(uint64_t value, uint8_t num_bits)
    {
        for (uint8_t b = num_bits; b > 0; b--) {
            range >>= 1;
            if ((value >> (b - 1)) & 1) low += range;
            normalize();
        }
    }

    /**
     * @brief encodes the lowest bits of a value (the same interface as range_decoder::code_bits)
     * @param value the value
     * @param num_bits number of bits
     * @return the value
     */
    inline uint64_t code_bits(uint64_t value, uint8_t num_bits)
    {
        encode_bits(value, num_bits);
        return value;
    }

    // writes the remaining bytes and returns the encoded bytes
    std::vector<uint8_t> finish()
    {
        for (uint32_t i = 0; i < sizeof(uint32_t) + 1; i++) shift_low();
        while (!out.empty() && out.back() == 0) out.pop_back();
        return std::move(out);
    }
};

/**
 * @brief decodes the bits encoded by range_encoder
 */
class range_decoder {
private:
    const uint8_t* pos = nullptr; // the next byte to read
    const uint8_t* end = nullptr; // the end of the encoded bytes
    uint32_t range = std::numeric_limits<uint32_t>::max(); // the length of the interval
    uint32_t offset = 0; // the read bits minus the lower end of the interval

    // returns the next byte (0 after the end)
    inline uint8_t next_byte() { return pos < end ? *pos++ : 0; }

    // shifts the range while it is smaller than range_coder_min_range
    inline void normalize()
    {
        while (range < range_coder_min_range) {
            range <<= range_coder_output_bits;
            offset = (offset << range_coder_output_bits) | next_byte();
        }
    }

public:
    range_decoder() = default;

    /**
     * @brief starts decoding bytes written by range_encoder
     * @param data the bytes
     * @param size number of bytes
     */
    range_decoder(const uint8_t* data, uint64_t size) : pos(data), end(data + size)
    {
        for (uint32_t i = 0; i < sizeof(uint32_t); i++) offset = (offset << range_coder_output_bits) | next_byte();
    }

    /**
     * @brief decodes a bit
     * @param p0 the probability of a 0-bit in units of 2^-probability_bits, as it was given to the encoder
     * @return the bit
     */
    inline bool decode(uint32_t p0)
    {
        const uint32_t bound = (range >> probability_bits) * p0;
        bool bit;

        if (offset < bound) {
            range = bound;
            bit = false;
        } else {
            offset -= bound;
            range -= bound;
            bit = true;
        }

        normalize();
        return bit;
    }

    /**
     * @brief decodes a bit (the same interface as range_encoder::code)
     * @param p0 the probability of a 0-bit in units of 2^-probability_bits, as it was given to the encoder
     * @return the bit
     */
    inline bool code(bool, uint32_t p0) { return decode(p0); }

    /**
     * @brief decodes bits encoded by range_encoder::encode_bits
     * @param num_bits number of bits
     * @return the value
     */
    inline uint64_t decode_bits(uint8_t num_bits)
    {
        uint64_t value = 0;

        for (uint8_t b = 0; b < num_bits; b++) {
            range >>= 1;
            const bool bit = offset >= range;
            if (bit) offset -= range;
            value = (value << 1) | uint64_t(bit);
            normalize();
        }

        return value;
    }

    /**
     * @brief decodes bits encoded by range_encoder::code_bits (the same interface as range_encoder::code_bits)
     * @param num_bits number of bits
     * @return the value
     */
    inline uint64_t code_bits(uint64_t, uint8_t num_bits) { return decode_bits(num_bits); }
};

#endif // STRING_UTILS_CODING_RANGE_CODER_HPP
