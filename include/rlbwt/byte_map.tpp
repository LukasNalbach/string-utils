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

#ifndef STRING_UTILS_RLBWT_BYTE_MAP_TPP
#define STRING_UTILS_RLBWT_BYTE_MAP_TPP

#include <rlbwt/byte_map.hpp>

#include <cstring>
#include <limits>
#include <string>

#include <omp.h>

#include <misc/bits.hpp>

namespace rlbwt {

inline byte_set occurring_bytes(const text_source<uint8_t>& text, uint64_t num_threads, phase_progress* progress)
{
    const uint64_t n = text.size();
    byte_set occurs {};
    if (n == 0) return occurs;
    const uint64_t blocks = div_ceil(n, read_block_size);

    #pragma omp parallel num_threads(std::max<uint64_t>(1, std::min(num_threads, blocks)))
    {
        const std::unique_ptr<text_source<uint8_t>::reader> reader = text.new_reader();
        byte_set seen {}; // the bytes this thread has read

        #pragma omp for schedule(dynamic)
        for (uint64_t b = 0; b < blocks; b++) {
            const uint64_t pos = b * read_block_size;
            const uint64_t len = std::min(read_block_size, n - pos);
            const uint8_t* block = reader->view(pos, len);
            for (uint64_t i = 0; i < len; i++) seen[block[i]] = true;
            if (progress != nullptr) progress->advance(len);
        }

        #pragma omp critical
        for (uint64_t c = 0; c < num_byte_values; c++) occurs[c] = occurs[c] || seen[c];
    }

    return occurs;
}

inline bool byte_map::assign(const byte_set& occurs, uint64_t min_symbol)
{
    smallest = min_symbol;
    bytes.clear();
    symbol.fill(0);
    for (uint64_t c = 0; c < num_byte_values; c++) if (occurs[c]) bytes.push_back(uint8_t(c));
    if (min_symbol > uint64_t(std::numeric_limits<uint16_t>::max()) + 1 - sigma()) return false;
    for (uint64_t i = 0; i < sigma(); i++) symbol[bytes[i]] = uint16_t(min_symbol + i);
    return true;
}

inline bool byte_map::load(const run_data_file& file)
{
    std::string section_bytes;
    uint64_t s = 0; // the symbol of the smallest byte
    if (!file.read_section(section_byte_map, section_bytes) || section_bytes.size() < sizeof(s)
        || section_bytes.size() - sizeof(s) > num_byte_values) {
        return false;
    }

    std::memcpy(&s, section_bytes.data(), sizeof(s));
    byte_set occurs {};

    for (uint64_t i = sizeof(s); i < section_bytes.size(); i++) {
        if (i > sizeof(s) && uint8_t(section_bytes[i]) <= uint8_t(section_bytes[i - 1])) return false;
        occurs[uint8_t(section_bytes[i])] = true;
    }

    return assign(occurs, s);
}

template <typename build_t>
auto build_with_byte_map(const byte_set& occurs, uint64_t min_symbol, output_params& prm, build_t build)
{
    if (!needs_byte_map(occurs, min_symbol)) {
        const std::array<uint8_t, num_byte_values>* no_map = nullptr;
        return build(no_map);
    }

    const byte_map map(occurs, min_symbol);
    prm.extra_sections.push_back(map.section());
    return map.with_symbols([&](const auto& symbols) { return build(&symbols); });
}

} // namespace rlbwt

#endif // STRING_UTILS_RLBWT_BYTE_MAP_TPP
