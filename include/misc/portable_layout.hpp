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

#ifndef STRING_UTILS_MISC_PORTABLE_LAYOUT_HPP
#define STRING_UTILS_MISC_PORTABLE_LAYOUT_HPP

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

#include <misc/bits.hpp>

/* [[no_unique_address]] on every ABI: the Microsoft C++ ABI (MSVC, and clang targeting *-windows-msvc) ignores
   [[no_unique_address]] and only knows [[msvc::no_unique_address]] */
#if defined(_MSC_VER)
#define STRING_UTILS_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define STRING_UTILS_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

/**
 * @brief a bit-field of a packed_bit_fields: the bits pos..pos+width-1, counted from bit 0 of byte 0
 * @tparam pos_ position of its lowest bit
 * @tparam width_ number of bits (1..64)
 */
template <uint64_t pos_, uint64_t width_>
struct bit_field_t {
    static_assert(width_ >= 1 && width_ <= 8 * sizeof(uint64_t));

    static constexpr uint64_t pos = pos_;
    static constexpr uint64_t width = width_;
    static constexpr uint64_t end = pos_ + width_; // position of the bit after the field
};

// position of the bit after the bit-field prev_field (0 for prev_field = void, i.e., before the first field)
template <typename prev_field>
inline constexpr uint64_t end_of_bit_field = prev_field::end;

template <>
inline constexpr uint64_t end_of_bit_field<void> = 0;

// the bit-field of width bits that follows the bit-field prev_field (the first field of a struct: prev_field = void)
template <typename prev_field, uint64_t width>
using next_bit_field_t = bit_field_t<end_of_bit_field<prev_field>, width>;

// number of bytes the bit-fields up to last_field (inclusive) take
template <typename last_field>
inline constexpr uint64_t bytes_of_bit_fields = div_ceil<uint64_t>(last_field::end, 8);

/**
 * @brief bit-fields in num_bytes bytes at the positions GCC and clang give the bit-fields of a packed struct on a
 *        little-endian Itanium-ABI target (e.g. x86-64 Linux): each field starts at the bit after the previous one,
 *        the first one at bit 0 of byte 0. The Microsoft ABI does not let a bit-field cross a storage unit of its type,
 *        so a struct whose bytes have to be the same on every ABI stores its bit-fields here (see bit_field_t).
 * @tparam num_bytes number of bytes
 */
template <uint64_t num_bytes>
struct packed_bit_fields {
    static_assert(std::endian::native == std::endian::little,
        "packed_bit_fields stores the fields in the order of a little-endian layout");

    uint8_t bytes[num_bytes];

protected:
    // number of bytes field occupies
    template <typename field>
    static constexpr uint64_t span_bytes = div_ceil<uint64_t>(field::pos % 8 + field::width, 8);

    // number of bytes read and written to access field: the smallest of 1, 2, 4 and 8 bytes that holds it, or all
    // bytes if there are fewer
    template <typename field>
    static constexpr uint64_t window_bytes = std::min<uint64_t>(num_bytes, std::bit_ceil(span_bytes<field>));

    // first byte of the bytes read and written to access field (they lie inside the struct)
    template <typename field>
    static constexpr uint64_t window_beg()
    {
        static_assert(field::end <= 8 * num_bytes, "the field does not fit into the bytes");
        static_assert(span_bytes<field> <= sizeof(uint64_t), "the field does not fit into one 64-bit word");
        return std::min<uint64_t>(field::pos / 8, num_bytes - window_bytes<field>);
    }

    // returns the bytes read to access field, as a little-endian word
    template <typename field>
    [[gnu::always_inline]] inline uint64_t read_window() const
    {
        uint64_t word = 0;
        std::memcpy(&word, bytes + window_beg<field>(), window_bytes<field>);
        return word;
    }

public:
    // returns the value of an unsigned field
    template <typename field>
    [[gnu::always_inline]] inline uint64_t get() const
    { return (read_window<field>() >> (field::pos - 8 * window_beg<field>())) & low_bits_mask(field::width); }

    // returns the value of a signed field (two's complement)
    template <typename field>
    [[gnu::always_inline]] inline int64_t get_signed() const
    {
        constexpr uint64_t unused_bits = 8 * sizeof(uint64_t) - field::width;
        return int64_t(get<field>() << unused_bits) >> unused_bits;
    }

    // sets a field to the lowest field::width bits of value
    template <typename field>
    [[gnu::always_inline]] inline void set(uint64_t value)
    {
        if constexpr (field::pos % 8 == 0 && field::width % 8 == 0) {
            // a field of whole bytes is written without reading the bytes around it
            std::memcpy(bytes + field::pos / 8, &value, field::width / 8);
        } else {
            constexpr uint64_t shift = field::pos - 8 * window_beg<field>();
            constexpr uint64_t mask = low_bits_mask(field::width) << shift;
            const uint64_t word = (read_window<field>() & ~mask) | ((value << shift) & mask);
            std::memcpy(bytes + window_beg<field>(), &word, window_bytes<field>);
        }
    }

protected:
    // the type of the value of a field passed to assign
    template <typename field>
    using value_of_t = uint64_t;

    // number of 64-bit words assign composes the bytes in
    static constexpr uint64_t num_words = div_ceil<uint64_t>(num_bytes, sizeof(uint64_t));

    // adds the lowest field::width bits of value at the position of field to the little-endian words
    template <typename field>
    [[gnu::always_inline]] static inline void add_to_words(uint64_t* words, uint64_t value)
    {
        constexpr uint64_t word_bits = 8 * sizeof(uint64_t);
        constexpr uint64_t idx = field::pos / word_bits, shift = field::pos % word_bits;
        value &= low_bits_mask(field::width);
        words[idx] |= value << shift;
        if constexpr (shift + field::width > word_bits) words[idx + 1] |= value >> (word_bits - shift);
    }

public:
    /**
     * @brief sets the given fields to the lowest bits of the values and all other bits to 0, with one write of the
     *        bytes
     * @tparam fields the fields
     * @param values their values, in the order of fields
     */
    template <typename... fields>
    [[gnu::always_inline]] inline void assign(value_of_t<fields>... values)
    {
        uint64_t words[num_words] = {};
        (add_to_words<fields>(words, values), ...);
        std::memcpy(bytes, words, num_bytes);
    }
};

#endif // STRING_UTILS_MISC_PORTABLE_LAYOUT_HPP
