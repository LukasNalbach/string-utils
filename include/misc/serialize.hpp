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

#ifndef STRING_UTILS_MISC_SERIALIZE_HPP
#define STRING_UTILS_MISC_SERIALIZE_HPP

#include <algorithm>
#include <climits>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include <misc/log.hpp>

// ############################# CHUNKED STREAM I/O #############################

/**
 * @brief reads size bytes from the input stream into data, in chunks that fit into a 32-bit signed integer
 * @param in the input stream
 * @param data the destination buffer (must hold at least size bytes)
 * @param size the number of bytes to read
 * @param prog counts the bytes read (see phase_progress); if given, the chunks have a size of 16 MiB
 */
inline void read_in_chunks(std::istream& in, char* data, uint64_t size,
    phase_progress* prog = nullptr)
{
    uint64_t size_left = size;
    uint64_t bytes_to_read;
    const uint64_t chunk = prog == nullptr ? uint64_t(INT_MAX) : 16 * 1024 * 1024;

    while (size_left > 0) {
        bytes_to_read = std::min<uint64_t>(size_left, chunk);
        in.read(data + (size - size_left), bytes_to_read);
        size_left -= bytes_to_read;
        if (prog != nullptr) prog->advance(bytes_to_read);
    }
}

/**
 * @brief writes size bytes from data to the output stream, in chunks that fit into a 32-bit signed integer
 * @param out the output stream
 * @param data the source buffer (must hold at least size bytes)
 * @param size the number of bytes to write
 * @param prog counts the bytes written (see phase_progress); if given, the chunks have a size of 16 MiB
 */
inline void write_in_chunks(std::ostream& out, const char* data, uint64_t size,
    phase_progress* prog = nullptr)
{
    uint64_t size_left = size;
    uint64_t bytes_to_write;
    const uint64_t chunk = prog == nullptr ? uint64_t(INT_MAX) : 16 * 1024 * 1024;

    while (size_left > 0) {
        bytes_to_write = std::min<uint64_t>(size_left, chunk);
        out.write(data + (size - size_left), bytes_to_write);
        size_left -= bytes_to_write;
        if (prog != nullptr) prog->advance(bytes_to_write);
    }
}

// ############################# SERIALIZATION #############################

/**
 * @brief writes the size (8 bytes) and the elements of a vector of trivially copyable values to a stream
 * @tparam val_t type of the elements
 * @param out the stream
 * @param v the vector
 */
template <typename val_t, typename alloc_t>
inline void write_vector(std::ostream& out, const std::vector<val_t, alloc_t>& v)
{
    uint64_t size = v.size();
    out.write((char*) &size, sizeof(uint64_t));
    if (size != 0) out.write((const char*) v.data(), size * sizeof(val_t));
}

/**
 * @brief reads a vector written by write_vector
 * @tparam val_t type of the elements
 * @param in the stream
 * @param v the vector to read into (resized)
 */
template <typename val_t, typename alloc_t>
inline void read_vector(std::istream& in, std::vector<val_t, alloc_t>& v)
{
    uint64_t size;
    in.read((char*) &size, sizeof(uint64_t));
    v.resize(size);
    if (size != 0) in.read((char*) v.data(), size * sizeof(val_t));
}

/**
 * @brief writes the length (8 bytes) and the characters of a string to a stream
 * @param out the stream
 * @param str the string
 */
inline void write_string(std::ostream& out, const std::string& str)
{
    const uint64_t length = str.size();
    out.write((const char*) &length, sizeof(uint64_t));
    out.write(str.data(), length);
}

/**
 * @brief reads a string written by write_string
 * @param in the stream
 * @param str the string to read into (resized)
 */
inline void read_string(std::istream& in, std::string& str)
{
    uint64_t length = 0;
    in.read((char*) &length, sizeof(uint64_t));
    str.resize(length);
    in.read(str.data(), length);
}

#endif // STRING_UTILS_MISC_SERIALIZE_HPP
