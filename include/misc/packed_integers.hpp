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

#ifndef STRING_UTILS_MISC_PACKED_INTEGERS_HPP
#define STRING_UTILS_MISC_PACKED_INTEGERS_HPP

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>

using uint128_t = __uint128_t;
using int128_t = __int128_t;

/**
 * @brief writes a 128-bit value in decimal
 * @param out output stream
 * @param value the value to write
 * @return the output stream
 */
inline std::ostream& operator<<(std::ostream& out, uint128_t value)
{
    if (value == 0) return out << '0';

    char digits[40];
    uint8_t len = 0;

    while (value != 0) {
        digits[len++] = char('0' + uint8_t(value % 10));
        value /= 10;
    }

    while (len != 0) out << digits[--len];

    return out;
}

/**
 * @brief an unsigned value in [0, 2^40) stored in 5 bytes, implicitly convertible to and from uint64_t
 */
struct uint40_t {
    static_assert(std::endian::native == std::endian::little,
        "uint40_t stores the low five bytes of a value, which assumes a little-endian layout");

    static constexpr uint8_t width = 5; // bytes per value
    static constexpr uint64_t max = (uint64_t { 1 } << (8 * width)) - 1; // largest storable value

    uint8_t bytes[width];

    uint40_t() = default;

    uint40_t(uint64_t value)
    {
        assert(value <= max);
        uint32_t low = uint32_t(value);
        std::memcpy(bytes, &low, 4);
        bytes[4] = uint8_t(value >> 32);
    }

    // returns the stored value
    operator uint64_t() const
    {
        uint32_t low;
        std::memcpy(&low, bytes, 4);
        return uint64_t(low) | (uint64_t(bytes[4]) << 32);
    }
};

static_assert(sizeof(uint40_t) == uint40_t::width);
static_assert(alignof(uint40_t) == 1);

/**
 * @brief a signed value in [-2^39, 2^39) stored in 5 bytes, implicitly convertible to and from int64_t
 */
struct int40_t {
    static_assert(std::endian::native == std::endian::little,
        "int40_t stores the low five bytes of a value, which assumes a little-endian layout");

    static constexpr uint8_t width = 5; // bytes per value
    static constexpr int64_t max = (int64_t { 1 } << (8 * width - 1)) - 1; // 2^39 - 1
    static constexpr int64_t min = -(int64_t { 1 } << (8 * width - 1));    // -2^39

    uint8_t bytes[width];

    int40_t() = default;

    int40_t(int64_t value)
    {
        assert(value >= min && value <= max);
        const uint32_t low = uint32_t(uint64_t(value));
        std::memcpy(bytes, &low, 4);
        bytes[4] = uint8_t(uint64_t(value) >> 32);
    }

    // returns the stored value
    operator int64_t() const
    {
        uint32_t low;
        std::memcpy(&low, bytes, 4);
        const uint64_t raw = uint64_t(low) | (uint64_t(bytes[4]) << 32);
        // sign-extend from bit 39
        return int64_t(raw << 24) >> 24;
    }

    friend bool operator==(const int40_t& a, const int40_t& b)
    { return std::memcmp(a.bytes, b.bytes, width) == 0; }
};

/**
 * @brief an unsigned value in [0, 2^48) stored in 6 bytes, implicitly convertible to and from uint64_t (used for
 *        values larger than max_narrow_value, see wide_values)
 */
struct uint48_t {
    static_assert(std::endian::native == std::endian::little,
        "uint48_t stores the low six bytes of a value, which assumes a little-endian layout");

    static constexpr uint8_t width = 6; // bytes per value
    static constexpr uint64_t max = (uint64_t { 1 } << (8 * width)) - 1; // largest storable value

    uint8_t bytes[width];

    uint48_t() = default;

    uint48_t(uint64_t value)
    {
        assert(value <= max);
        const uint32_t low = uint32_t(value);
        const uint16_t high = uint16_t(value >> 32);
        std::memcpy(bytes, &low, 4);
        std::memcpy(bytes + 4, &high, 2);
    }

    // returns the stored value
    operator uint64_t() const
    {
        uint32_t low;
        uint16_t high;
        std::memcpy(&low, bytes, 4);
        std::memcpy(&high, bytes + 4, 2);
        return uint64_t(low) | (uint64_t(high) << 32);
    }
};

static_assert(sizeof(uint48_t) == uint48_t::width);
static_assert(alignof(uint48_t) == 1);

/**
 * @brief a signed value in [-2^47, 2^47) stored in 6 bytes, implicitly convertible to and from int64_t (used for
 *        values whose absolute value is larger than max_narrow_value / 2, see wide_signed_values)
 */
struct int48_t {
    static_assert(std::endian::native == std::endian::little,
        "int48_t stores the low six bytes of a value, which assumes a little-endian layout");

    static constexpr uint8_t width = 6; // bytes per value
    static constexpr int64_t max = (int64_t { 1 } << (8 * width - 1)) - 1; // 2^47 - 1
    static constexpr int64_t min = -(int64_t { 1 } << (8 * width - 1));    // -2^47

    uint8_t bytes[width];

    int48_t() = default;

    int48_t(int64_t value)
    {
        assert(value >= min && value <= max);
        const uint32_t low = uint32_t(uint64_t(value));
        const uint16_t high = uint16_t(uint64_t(value) >> 32);
        std::memcpy(bytes, &low, 4);
        std::memcpy(bytes + 4, &high, 2);
    }

    // returns the stored value
    operator int64_t() const
    {
        uint32_t low;
        uint16_t high;
        std::memcpy(&low, bytes, 4);
        std::memcpy(&high, bytes + 4, 2);
        const uint64_t raw = uint64_t(low) | (uint64_t(high) << 32);
        // sign-extend from bit 47
        return int64_t(raw << 16) >> 16;
    }

    friend bool operator==(const int48_t& a, const int48_t& b)
    { return std::memcmp(a.bytes, b.bytes, width) == 0; }
};

// the largest value that is stored with 40 bits (uint40_t); larger values are stored with 48 bits (uint48_t); a
// build can set it lower to test the 48-bit variants (see cmake/settings.cmake)
#ifndef STRING_UTILS_MAX_NARROW_VALUE
#define STRING_UTILS_MAX_NARROW_VALUE ((uint64_t { 1 } << 40) - 1)
#endif

inline constexpr uint64_t max_narrow_value = STRING_UTILS_MAX_NARROW_VALUE;
static_assert(max_narrow_value <= uint40_t::max, "STRING_UTILS_MAX_NARROW_VALUE must fit into uint40_t");
static_assert(max_narrow_value / 2 <= uint64_t(int40_t::max), "STRING_UTILS_MAX_NARROW_VALUE / 2 must fit into int40_t");

// returns whether the values in [0, max_value] are stored with 48 bits (uint48_t) instead of 40 bits (uint40_t)
inline constexpr bool wide_values(uint64_t max_value) { return max_value > max_narrow_value; }

// returns whether the values in [-max_abs, max_abs] are stored with 48 bits (int48_t) instead of 40 bits (int40_t)
inline constexpr bool wide_signed_values(uint64_t max_abs) { return max_abs > max_narrow_value / 2; }

/**
 * @brief returns the largest value of an unsigned integer type
 * @tparam value_t a built-in unsigned integer type, or a packed one with a member max (e.g. uint40_t or uint48_t)
 */
template <typename value_t>
inline constexpr uint64_t max_value_of()
{
    if constexpr (requires { value_t::max; }) return uint64_t(value_t::max);
    else return uint64_t(std::numeric_limits<value_t>::max());
}

#endif // STRING_UTILS_MISC_PACKED_INTEGERS_HPP
