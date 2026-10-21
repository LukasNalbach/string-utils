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

#ifndef STRING_UTILS_MISC_BITS_HPP
#define STRING_UTILS_MISC_BITS_HPP

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <misc/packed_integers.hpp>

/**
 * @brief computes the ceiling of x / y
 * @tparam int_t integer type (signed or unsigned)
 * @param x the dividend
 * @param y the divisor (not 0)
 * @return ceil(x / y)
 */
template <typename int_t>
inline constexpr int_t div_ceil(int_t x, int_t y)
{
    if constexpr (std::is_signed_v<int_t>) {
        const int_t quotient = x / y;
        return (x % y != 0 && ((x < 0) == (y < 0))) ? quotient + 1 : quotient;
    } else {
        return x == 0 ? 0 : (1 + (x - 1) / y);
    }
}

/**
 * @brief returns the mask of the lowest width bits of a 64-bit word
 * @param width number of bits (all 64 bits for width >= 64)
 * @return 2^width - 1 for width < 64, else 2^64 - 1
 */
inline constexpr uint64_t low_bits_mask(uint64_t width)
{
    return width >= 64 ? ~uint64_t { 0 } : (uint64_t { 1 } << width) - 1;
}

/**
 * @brief computes the floor of x / y
 * @tparam int_t integer type (signed or unsigned)
 * @param x the dividend
 * @param y the divisor (not 0)
 * @return floor(x / y)
 */
template <typename int_t>
inline constexpr int_t div_floor(int_t x, int_t y)
{
    if constexpr (std::is_signed_v<int_t>) {
        const int_t quotient = x / y;
        return (x % y != 0 && ((x < 0) != (y < 0))) ? quotient - 1 : quotient;
    } else {
        return x / y;
    }
}

/**
 * @brief maps a signed value to an unsigned value (zigzag encoding); values with a small absolute value are
 *        mapped to small values
 * @param val a signed value
 * @return 2 * val for a non-negative value, 2 * |val| - 1 for a negative one
 */
inline constexpr uint64_t zigzag(int64_t val)
{
    return val >= 0 ? uint64_t(val) * 2 : uint64_t(-(val + 1)) * 2 + 1;
}

/**
 * @brief inverts zigzag
 * @param val a zigzag-encoded value
 * @return the signed value encoded by val
 */
inline constexpr int64_t unzigzag(uint64_t val)
{
    return (val & 1) ? -int64_t((val >> 1) + 1) : int64_t(val >> 1);
}

/**
 * @brief counts the number of set bits in x
 * @tparam word_t word type (uint32_t, uint64_t or uint128_t)
 * @param x a word
 * @return the number of set bits in x
 */
template <typename word_t>
inline int32_t popcount(word_t x)
{
    static_assert(std::is_same_v<word_t, uint32_t> ||
                  std::is_same_v<word_t, uint64_t> ||
                  std::is_same_v<word_t, uint128_t>);

    if constexpr (std::is_same_v<word_t, uint128_t>) {
        uint64_t high = x >> 64;
        uint64_t low = x;
        return __builtin_popcountll(high) + __builtin_popcountll(low);
    } else if constexpr (std::is_same_v<word_t, uint64_t>) {
        return __builtin_popcountll(x);
    } else {
        return __builtin_popcount(x);
    }
}

/**
 * @brief reads a value from a bit-packed array of 64-bit words (the value can span two words)
 * @tparam alloc_t allocator type of the array
 * @param data the array
 * @param bit position of the first bit of the value
 * @param width number of bits of the value (for a width of 0, 0 is returned)
 * @return the value
 */
template <typename alloc_t>
inline uint64_t read_bits(const std::vector<uint64_t, alloc_t>& data, uint64_t bit, uint8_t width)
{
    if (width == 0) [[unlikely]] return 0;
    uint64_t word = bit >> 6;
    uint8_t shift = uint8_t(bit & 63);
    uint64_t value = data[word] >> shift;
    if (shift + width > 64) value |= data[word + 1] << (64 - shift);
    return value & (~uint64_t { 0 } >> (64 - width));
}

/**
 * @brief writes a value to a bit-packed array of 64-bit words (the value can span two words)
 * @tparam alloc_t allocator type of the array
 * @param data the array; the bits the value is written to must be 0 (the value is written with OR)
 * @param bit position of the first bit of the value
 * @param width number of bits of the value (for a width of 0, nothing is written)
 * @param value the value (must fit into width bits)
 */
template <typename alloc_t>
inline void write_bits(std::vector<uint64_t, alloc_t>& data, uint64_t bit, uint8_t width, uint64_t value)
{
    if (width == 0) [[unlikely]] return;
    uint64_t word = bit >> 6;
    uint8_t shift = uint8_t(bit & 63);
    data[word] |= value << shift;
    if (shift + width > 64) data[word + 1] |= value >> (64 - shift);
}

/**
 * @brief returns the bit pattern of a symbol, zero-extended to 64 bits (a signed symbol is not sign-extended)
 * @tparam sym_t an integral type of at most 64 bits, or an unsigned packed one
 * @param sym a symbol
 * @return the bit pattern of sym
 */
template <typename sym_t>
inline uint64_t symbol_bit_pattern(sym_t sym)
{
    if constexpr (std::is_signed_v<sym_t>) {
        std::make_unsigned_t<sym_t> unsigned_sym;
        std::memcpy(&unsigned_sym, &sym, sizeof(sym_t));
        return uint64_t(unsigned_sym);
    } else {
        return uint64_t(sym);
    }
}

#endif // STRING_UTILS_MISC_BITS_HPP
