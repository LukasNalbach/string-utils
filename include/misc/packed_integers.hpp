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

#include <algorithm>
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

// the built-in unsigned integer type of `bytes` bytes (1, 2, 4 or 8)
template <uint8_t bytes>
using builtin_uint_t = std::conditional_t<bytes == sizeof(uint8_t), uint8_t,
    std::conditional_t<bytes == sizeof(uint16_t), uint16_t,
    std::conditional_t<bytes == sizeof(uint32_t), uint32_t, uint64_t>>>;

/**
 * @brief returns the little-endian value of the bytes src[0..bytes-1]
 * @tparam bytes number of bytes, in [1, 8]
 */
template <uint8_t bytes>
inline uint64_t load_packed_bytes(const uint8_t* src)
{
    static_assert(std::endian::native == std::endian::little,
        "load_packed_bytes reads the low bytes of a value, which assumes a little-endian layout");
    // the bytes are read in parts of a built-in width, the widest first
    constexpr uint8_t low = std::bit_floor(bytes);
    builtin_uint_t<low> value;
    std::memcpy(&value, src, low);
    if constexpr (low == bytes) return value;
    else return uint64_t(value) | (load_packed_bytes<bytes - low>(src + low) << (8 * low));
}

/**
 * @brief stores the low `bytes` bytes of value in dst[0..bytes-1] (little endian)
 * @tparam bytes number of bytes, in [1, 8]
 */
template <uint8_t bytes>
inline void store_packed_bytes(uint8_t* dst, uint64_t value)
{
    static_assert(std::endian::native == std::endian::little,
        "store_packed_bytes writes the low bytes of a value, which assumes a little-endian layout");
    constexpr uint8_t low = std::bit_floor(bytes);
    const builtin_uint_t<low> part = builtin_uint_t<low>(value);
    std::memcpy(dst, &part, low);
    if constexpr (low != bytes) store_packed_bytes<bytes - low>(dst + low, value >> (8 * low));
}

/**
 * @brief an unsigned value in [0, 2^(8 * num_bytes)) stored in num_bytes bytes, implicitly convertible to and from
 *        uint64_t
 * @tparam num_bytes number of bytes, in [1, 7]
 */
template <uint8_t num_bytes>
struct packed_unsigned {
    static_assert(0 < num_bytes && num_bytes < sizeof(uint64_t), "a packed_unsigned has 1 to 7 bytes");

    static constexpr uint8_t width = num_bytes; // bytes per value
    static constexpr uint64_t max = (uint64_t { 1 } << (8 * width)) - 1; // largest storable value

    uint8_t bytes[width];

    packed_unsigned() = default;

    packed_unsigned(uint64_t value)
    {
        assert(value <= max);
        store_packed_bytes<width>(bytes, value);
    }

    // returns the stored value
    operator uint64_t() const { return load_packed_bytes<width>(bytes); }
};

/**
 * @brief a signed value in [-2^(8 * num_bytes - 1), 2^(8 * num_bytes - 1)) stored in num_bytes bytes, implicitly
 *        convertible to and from int64_t
 * @tparam num_bytes number of bytes, in [1, 7]
 */
template <uint8_t num_bytes>
struct packed_signed {
    static_assert(0 < num_bytes && num_bytes < sizeof(int64_t), "a packed_signed has 1 to 7 bytes");

    static constexpr uint8_t width = num_bytes; // bytes per value
    static constexpr int64_t max = (int64_t { 1 } << (8 * width - 1)) - 1; // 2^(8 * width - 1) - 1
    static constexpr int64_t min = -(int64_t { 1 } << (8 * width - 1));    // -2^(8 * width - 1)

    uint8_t bytes[width];

    packed_signed() = default;

    packed_signed(int64_t value)
    {
        assert(value >= min && value <= max);
        store_packed_bytes<width>(bytes, uint64_t(value));
    }

    // returns the stored value
    operator int64_t() const
    {
        // sign-extend from bit 8 * width - 1
        constexpr uint8_t unused_bits = 8 * (sizeof(int64_t) - width);
        return int64_t(load_packed_bytes<width>(bytes) << unused_bits) >> unused_bits;
    }

    friend bool operator==(const packed_signed& a, const packed_signed& b)
    { return std::memcmp(a.bytes, b.bytes, width) == 0; }
};

using uint24_t = packed_unsigned<3>;
using uint40_t = packed_unsigned<5>;
using uint48_t = packed_unsigned<6>;
using uint56_t = packed_unsigned<7>;

using int24_t = packed_signed<3>;
using int40_t = packed_signed<5>;
using int48_t = packed_signed<6>;
using int56_t = packed_signed<7>;

static_assert(sizeof(uint40_t) == uint40_t::width && alignof(uint40_t) == 1);
static_assert(sizeof(uint48_t) == uint48_t::width && alignof(uint48_t) == 1);

// true <=> value_t is a packed unsigned integer type (see packed_unsigned)
template <typename value_t> inline constexpr bool is_packed_unsigned_v = false;
template <uint8_t num_bytes> inline constexpr bool is_packed_unsigned_v<packed_unsigned<num_bytes>> = true;

// the unsigned integer type of `bytes` bytes, in [1, 8]: a built-in type (uint8_t, uint16_t, uint32_t or uint64_t)
// or a packed one (uint24_t, uint40_t, uint48_t or uint56_t)
template <uint8_t bytes>
using uint_of_width_t = std::conditional_t<std::has_single_bit(bytes), builtin_uint_t<bytes>, packed_unsigned<bytes>>;

// returns the fewest whole bytes (at least one) that hold the value x
inline constexpr uint8_t byte_width(uint64_t x) { return uint8_t(std::max<int>(1, (std::bit_width(x) + 7) / 8)); }

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
