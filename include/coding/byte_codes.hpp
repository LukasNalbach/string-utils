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
 * @file byte_codes.hpp
 * @brief integer codes of compressed files: variable-length integers and bit-packed values
 */

#ifndef STRING_UTILS_CODING_BYTE_CODES_HPP
#define STRING_UTILS_CODING_BYTE_CODES_HPP

#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <vector>

// number of distinct byte values
inline constexpr uint64_t num_byte_values = uint64_t(std::numeric_limits<uint8_t>::max()) + 1;

// number of value bits of a byte of a variable-length integer (the highest bit marks that another byte follows)
inline constexpr uint8_t varint_value_bits = std::numeric_limits<uint8_t>::digits - 1;

// the highest bit of a byte of a variable-length integer
inline constexpr uint8_t varint_continue = uint8_t(1) << varint_value_bits;

// returns the exception thrown when a compressed file is not valid
inline std::runtime_error invalid_file() { return std::runtime_error("invalid compressed file"); }

/**
 * @brief appends a variable-length integer: the value in groups of varint_value_bits bits, lowest group first, one
 *        byte per group; the highest bit of a byte is 1 iff another byte follows
 * @param out the bytes
 * @param value the value
 */
inline void write_varint(std::vector<uint8_t>& out, uint64_t value)
{
    while (value >= varint_continue) {
        out.push_back(uint8_t(value) | varint_continue);
        value >>= varint_value_bits;
    }

    out.push_back(uint8_t(value));
}

/**
 * @brief reads a variable-length integer (see write_varint)
 * @param in a reader with the method byte(), which returns the next byte
 */
template <typename reader_t>
uint64_t read_varint(reader_t& in)
{
    uint64_t value = 0;

    for (uint8_t shift = 0;; shift += varint_value_bits) {
        const uint8_t b = in.byte();
        if (shift >= std::numeric_limits<uint64_t>::digits) throw invalid_file();
        value |= uint64_t(b & (varint_continue - 1)) << shift;
        if ((b & varint_continue) == 0) return value;
    }
}

/**
 * @brief reads bytes of the compressed file; throws std::runtime_error when reading beyond the end
 */
class byte_reader {
private:
    const uint8_t* pos; // the next byte
    const uint8_t* end; // the end of the bytes

public:
    /**
     * @brief starts reading bytes
     * @param data the bytes
     * @param size number of bytes
     */
    byte_reader(const uint8_t* data, uint64_t size) : pos(data), end(data + size) { }

    // returns the next byte
    uint8_t byte()
    {
        if (pos == end) throw std::runtime_error("the compressed file is truncated");
        return *pos++;
    }

    // returns the next variable-length integer (see write_varint)
    uint64_t varint() { return read_varint(*this); }

    /**
     * @brief returns the next bytes and skips them
     * @param size number of bytes
     */
    const uint8_t* bytes(uint64_t size)
    {
        if (uint64_t(end - pos) < size) throw std::runtime_error("the compressed file is truncated");
        const uint8_t* first = pos;
        pos += size;
        return first;
    }

    // returns the number of bytes that have not been read
    uint64_t remaining() const { return uint64_t(end - pos); }
};

/**
 * @brief reads bytes of a compressed file from a stream, with the methods of byte_reader; throws std::runtime_error
 *        when reading beyond the end
 */
class stream_byte_reader {
private:
    std::istream& in; // the stream
    std::vector<uint8_t> buffer; // the bytes returned by the last call of bytes()

public:
    // starts reading bytes at the current position of a stream
    explicit stream_byte_reader(std::istream& in) : in(in) { }

    // returns the next byte
    uint8_t byte()
    {
        const auto c = in.get();
        if (c == std::istream::traits_type::eof()) throw std::runtime_error("the compressed file is truncated");
        return uint8_t(c);
    }

    // returns the next variable-length integer (see write_varint)
    uint64_t varint() { return read_varint(*this); }

    /**
     * @brief returns the next bytes; they stay valid until the next call
     * @param size number of bytes
     */
    const uint8_t* bytes(uint64_t size)
    {
        buffer.resize(size);
        in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(size));
        if (uint64_t(in.gcount()) != size) throw std::runtime_error("the compressed file is truncated");
        return buffer.data();
    }
};

/**
 * @brief appends values of given widths to bytes, lowest bit first; the last byte is padded with 0-bits
 */
class bit_packer {
private:
    std::vector<uint8_t>& out; // the bytes
    uint64_t buffer = 0; // the bits that have not been appended to out
    uint8_t num_bits = 0; // the number of bits in buffer

public:
    explicit bit_packer(std::vector<uint8_t>& out) : out(out) { }

    /**
     * @brief appends a value
     * @param value the value (less than 2^width)
     * @param width its number of bits, at most 64
     */
    void push(uint64_t value, uint8_t width)
    {
        for (uint8_t i = 0; i < width; i++) {
            buffer |= ((value >> i) & 1) << num_bits;

            if (++num_bits == std::numeric_limits<uint8_t>::digits) {
                out.push_back(uint8_t(buffer));
                buffer = 0;
                num_bits = 0;
            }
        }
    }

    // appends the last byte, if it holds bits
    void finish()
    {
        if (num_bits > 0) out.push_back(uint8_t(buffer));
        buffer = 0;
        num_bits = 0;
    }
};

/**
 * @brief reads the values written by bit_packer
 */
class bit_unpacker {
private:
    const uint8_t* data; // the bytes
    uint64_t bit = 0; // the next bit to read

public:
    explicit bit_unpacker(const uint8_t* data) : data(data) { }

    /**
     * @brief reads a value
     * @param width its number of bits, at most 64
     */
    uint64_t pop(uint8_t width)
    {
        uint64_t value = 0;

        for (uint8_t i = 0; i < width; i++, bit++) {
            const uint8_t byte = data[bit / std::numeric_limits<uint8_t>::digits];
            value |= uint64_t((byte >> (bit % std::numeric_limits<uint8_t>::digits)) & 1) << i;
        }

        return value;
    }
};

/**
 * @brief returns the number of bytes of count values of width bits written by bit_packer
 * @param count the number of values
 * @param width the number of bits of a value
 */
inline uint64_t packed_bytes(uint64_t count, uint8_t width)
{
    constexpr uint64_t byte_bits = std::numeric_limits<uint8_t>::digits;
    return (count * width + byte_bits - 1) / byte_bits;
}

#endif // STRING_UTILS_CODING_BYTE_CODES_HPP
