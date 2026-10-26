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

#ifndef STRING_UTILS_RLBWT_TEXT_SOURCES_TPP
#define STRING_UTILS_RLBWT_TEXT_SOURCES_TPP

#include <rlbwt/text_sources.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <misc/files.hpp>

namespace rlbwt {

// a reader of a file_source; it reads read_block_size bytes at once
template <typename sym_t>
class file_reader : public text_source<sym_t>::reader {
private:
    array_file_reader file; // reads the file
    uint64_t n; // n, the number of symbols in the file
    bool reverse; // true <=> the text is the reversed file
    const std::array<uint8_t, 256>* map; // the map of the bytes of the file_source (nullptr: not mapped)

public:
    file_reader(const std::string& path, uint64_t n, bool reverse, const std::array<uint8_t, 256>* map)
        : file(path, sizeof(sym_t), read_block_size), n(n), reverse(reverse), map(map)
    { }

    // without reversing or mapping, the symbols are viewed in the buffer of file
    const sym_t* view(uint64_t pos, uint64_t len) override
    {
        if (reverse || map != nullptr) return text_source<sym_t>::reader::view(pos, len);
        return (const sym_t*) file.view(pos, pos + len);
    }

    void read(uint64_t pos, uint64_t len, sym_t* dst) override
    {
        const uint64_t beg = reverse ? n - pos - len : pos;
        file.read(beg, beg + len, dst);
        if (reverse) std::reverse(dst, dst + len);
        if (map != nullptr) for (uint64_t i = 0; i < len; i++) dst[i] = (*map)[dst[i]];
    }
};

// a reader of a memory_source
template <typename sym_t>
class memory_reader : public text_source<sym_t>::reader {
private:
    const sym_t* text; // text[0..n-1]
    uint64_t n; // n, the number of symbols
    bool reverse; // true <=> the text is text[0..n-1] reversed

public:
    memory_reader(const sym_t* text, uint64_t n, bool reverse) : text(text), n(n), reverse(reverse) { }

    const sym_t* view(uint64_t pos, uint64_t len) override
    {
        return reverse ? text_source<sym_t>::reader::view(pos, len) : text + pos;
    }

    void read(uint64_t pos, uint64_t len, sym_t* dst) override
    {
        if (reverse) std::reverse_copy(text + n - pos - len, text + n - pos, dst);
        else std::memcpy(dst, text + pos, len * sizeof(sym_t));
    }
};

template <typename sym_t>
file_source<sym_t>::file_source(const std::string& path, bool reverse, const std::array<uint8_t, 256>* map)
    : path(path), reverse(reverse), mapped(map != nullptr)
{
    std::error_code ec;
    const uint64_t bytes = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot read " + path);
    if (bytes % sizeof(sym_t) != 0)
        throw std::invalid_argument("the file size is not a multiple of the symbol width");
    if (mapped && sizeof(sym_t) != 1)
        throw std::invalid_argument("a map is only applied to one-byte symbols");
    n = bytes / sizeof(sym_t);
    if (mapped) this->map = *map;
}

} // namespace rlbwt

#endif // STRING_UTILS_RLBWT_TEXT_SOURCES_TPP
