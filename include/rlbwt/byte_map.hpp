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
 * @file byte_map.hpp
 * @brief the map of the distinct bytes of a text to consecutive symbols (byte_map) and a byte text read through it
 *        (mapped_byte_source)
 */

#ifndef STRING_UTILS_RLBWT_BYTE_MAP_HPP
#define STRING_UTILS_RLBWT_BYTE_MAP_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <vector>

#include <coding/byte_codes.hpp>
#include <misc/log.hpp>
#include <rlbwt/run_data_file.hpp>
#include <rlbwt/types.hpp>

namespace rlbwt {

// [0..255] occurs[c] = whether the byte c occurs in a text
using byte_set = std::array<bool, num_byte_values>;

/**
 * @brief returns the bytes that occur in a text
 * @param text the text
 * @param num_threads the number of threads
 * @param progress if not nullptr, counts the bytes read
 */
inline byte_set occurring_bytes(const text_source<uint8_t>& text, uint64_t num_threads,
    phase_progress* progress = nullptr);

/**
 * @brief returns whether a byte below min_symbol occurs in a text
 * @param occurs the bytes that occur in the text
 * @param min_symbol the smallest symbol a construction accepts
 */
inline bool needs_byte_map(const byte_set& occurs, uint64_t min_symbol)
{
    return std::any_of(occurs.begin(), occurs.begin() + std::min(min_symbol, num_byte_values), [](bool c) { return c; });
}

/**
 * @brief the map of the distinct bytes of a text to the symbols s, s + 1, ... in increasing order (s: the symbol of the
 *        smallest byte), stored in the runs file as the section section_byte_map
 */
class byte_map {
private:
    uint64_t smallest = 0; // the symbol s of the smallest byte
    std::vector<uint8_t> bytes; // [0..sigma-1] the distinct bytes in increasing order
    std::array<uint16_t, num_byte_values> symbol {}; // [0..255] symbol[c] = the symbol of the byte c (0: c does not occur)

    // sets the map of the bytes that occur, the smallest to min_symbol; returns whether the symbols fit into 16 bits
    bool assign(const byte_set& occurs, uint64_t min_symbol);

public:
    byte_map() = default;

    /**
     * @brief the map of the bytes of a text
     * @param occurs the bytes that occur in the text
     * @param min_symbol the symbol s of the smallest byte
     * @throws std::invalid_argument if the symbols do not fit into 16 bits
     */
    byte_map(const byte_set& occurs, uint64_t min_symbol)
    {
        if (!assign(occurs, min_symbol)) throw std::invalid_argument("byte_map: the symbols do not fit into 16 bits");
    }

    // returns the number sigma of distinct bytes
    uint64_t sigma() const { return bytes.size(); }

    // returns the symbol s of the smallest byte
    uint64_t min_symbol() const { return smallest; }

    // returns the symbol of the byte c (0 if c does not occur)
    uint64_t symbol_of(uint8_t c) const { return symbol[c]; }

    // returns the byte of the symbol x in [s..s+sigma-1]
    uint8_t byte_of(uint64_t x) const { return bytes[x - smallest]; }

    // returns whether every symbol fits into one byte
    bool fits_in_byte() const { return smallest + sigma() <= num_byte_values; }

    // returns [0..255] the symbols of the bytes as values of type sym_t (see symbol_of)
    template <typename sym_t>
    std::array<sym_t, num_byte_values> symbols() const
    {
        std::array<sym_t, num_byte_values> map;
        for (uint64_t c = 0; c < num_byte_values; c++) map[c] = sym_t(symbol[c]);
        return map;
    }

    /**
     * @brief calls fnc(map) with [0..255] map[c] = the symbol of the byte c, a std::array of uint8_t if every symbol
     *        fits into one byte (see fits_in_byte) and of uint16_t else
     * @return the result of fnc
     */
    template <typename fnc_t>
    auto with_symbols(fnc_t fnc) const
    {
        if (fits_in_byte()) return fnc(symbols<uint8_t>());
        return fnc(symbols<uint16_t>());
    }

    // returns the section section_byte_map of the runs file
    extra_section section() const
    {
        return { section_byte_map, [s = smallest, bytes = bytes](std::ostream& out) {
            out.write((const char*) &s, sizeof(s));
            out.write((const char*) bytes.data(), std::streamsize(bytes.size()));
        } };
    }

    /**
     * @brief reads the section section_byte_map of a runs file
     * @param file the runs file
     * @return whether the file stores the section with distinct bytes in increasing order and symbols that fit into
     *         16 bits
     */
    bool load(const run_data_file& file);
};

/**
 * @brief a byte text whose bytes are read as symbols of type sym_t (see byte_map::with_symbols)
 */
template <typename sym_t>
class mapped_byte_source : public text_source<sym_t> {
private:
    const text_source<uint8_t>& text; // the byte text
    std::array<sym_t, num_byte_values> map; // [0..255] map[c] = the symbol of the byte c

    // reads the bytes of the byte text in blocks of at most read_block_size bytes and maps them
    class reader : public text_source<sym_t>::reader {
    private:
        const mapped_byte_source& source; // the mapped text
        std::unique_ptr<text_source<uint8_t>::reader> bytes; // reads the byte text

    public:
        explicit reader(const mapped_byte_source& source) : source(source), bytes(source.text.new_reader()) { }

        void read(uint64_t pos, uint64_t len, sym_t* dst) override
        {
            for (uint64_t i = 0; i < len; i += read_block_size) {
                const uint64_t block_len = std::min(read_block_size, len - i);
                const uint8_t* block = bytes->view(pos + i, block_len);
                for (uint64_t j = 0; j < block_len; j++) dst[i + j] = source.map[block[j]];
            }
        }
    };

public:
    /**
     * @brief a byte text with its bytes mapped
     * @param text the byte text (it must stay valid while the object is used)
     * @param map [0..255] map[c] = the symbol of the byte c
     */
    mapped_byte_source(const text_source<uint8_t>& text, const std::array<sym_t, num_byte_values>& map)
        : text(text), map(map) { }

    uint64_t size() const override { return text.size(); }

    std::unique_ptr<typename text_source<sym_t>::reader> new_reader() const override
    {
        return std::make_unique<reader>(*this);
    }
};

/**
 * @brief calls build(map) to build the output of a byte text: map is nullptr if every byte of the text is at least
 *        min_symbol, and else points to [0..255] the symbols of the bytes of a byte_map whose smallest symbol is
 *        min_symbol (a std::array of uint8_t or of uint16_t, see byte_map::with_symbols), whose section is added to
 *        prm.extra_sections before
 * @param occurs the bytes that occur in the text
 * @param min_symbol the smallest symbol the construction accepts
 * @param prm the parameters of the construction
 * @param build builds the output of the text read through map
 * @return the result of build
 */
template <typename build_t>
auto build_with_byte_map(const byte_set& occurs, uint64_t min_symbol, output_params& prm, build_t build);

} // namespace rlbwt

#include <rlbwt/byte_map.tpp>

#endif // STRING_UTILS_RLBWT_BYTE_MAP_HPP
