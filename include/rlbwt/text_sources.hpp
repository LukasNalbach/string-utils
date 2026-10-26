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
 * @file text_sources.hpp
 * @brief a text in a file (file_source) and a text in memory (memory_source)
 */

#ifndef STRING_UTILS_RLBWT_TEXT_SOURCES_HPP
#define STRING_UTILS_RLBWT_TEXT_SOURCES_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <rlbwt/types.hpp>

namespace rlbwt {

// the readers of a file_source and of a memory_source (see text_sources.tpp)
template <typename sym_t>
class file_reader;
template <typename sym_t>
class memory_reader;

/**
 * @brief a text stored in a file (sizeof(sym_t) bytes per symbol, little endian), optionally read in reverse and,
 *        for one-byte symbols, with its bytes mapped
 */
template <typename sym_t = uint8_t>
class file_source : public text_source<sym_t> {
private:
    std::string path; // path of the file
    uint64_t n = 0; // n, the number of symbols in the file
    bool reverse = false; // true <=> the text is the reversed file
    bool mapped = false; // true <=> map is applied to each byte
    // the map of the bytes: [0..255] map[c] = text symbol of byte c (only used if mapped)
    std::array<uint8_t, 256> map {};

public:
    /**
     * @brief opens a text file
     * @param path path of the file
     * @param reverse true <=> the text is the reversed file
     * @param map if not nullptr, the text symbol of byte c is (*map)[c] (only for one-byte symbols)
     */
    file_source(const std::string& path, bool reverse = false, const std::array<uint8_t, 256>* map = nullptr);

    uint64_t size() const override { return n; }

    std::unique_ptr<typename text_source<sym_t>::reader> new_reader() const override
    {
        return std::make_unique<file_reader<sym_t>>(path, n, reverse, mapped ? &map : nullptr);
    }
};

/**
 * @brief a text stored in memory, optionally read in reverse
 */
template <typename sym_t = uint8_t>
class memory_source : public text_source<sym_t> {
private:
    const sym_t* text; // text[0..n-1]
    uint64_t n; // n, the number of symbols
    bool reverse; // true <=> the text is text[0..n-1] reversed

public:
    /**
     * @brief a text in memory
     * @param text the symbols (not copied; they must stay valid while the text is read)
     * @param n number of symbols
     * @param reverse true <=> the text is text[0..n-1] reversed
     */
    memory_source(const sym_t* text, uint64_t n, bool reverse = false) : text(text), n(n), reverse(reverse) { }

    uint64_t size() const override { return n; }

    std::unique_ptr<typename text_source<sym_t>::reader> new_reader() const override
    {
        return std::make_unique<memory_reader<sym_t>>(text, n, reverse);
    }
};

} // namespace rlbwt

#include <rlbwt/text_sources.tpp>

#endif // STRING_UTILS_RLBWT_TEXT_SOURCES_HPP
