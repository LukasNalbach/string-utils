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

#ifndef STRING_UTILS_TESTS_TEST_FASTA_HPP
#define STRING_UTILS_TESTS_TEST_FASTA_HPP

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <coding/byte_codes.hpp>
#include <fasta/position_map.hpp>
#include <fasta/restore.hpp>
#include <fasta/scan.hpp>
#include <fasta/split.hpp>
#include <misc/compressor_cli.hpp>
#include <misc/log.hpp>
#include <misc/memory.hpp>
#include <misc/repetitive_input.hpp>
#include "test-inputs.hpp"

/**
 * @brief generates a random FASTA file: header lines that are a fixed prefix with the number of the record, the header
 *        before with one byte replaced, or random bytes; the sequences of the records are consecutive pieces of a
 *        repetitive sequence of at most max_size bytes (also none) over a random alphabet of at most max_sigma bytes
 *        without the line feed, with runs of one byte and lowercase runs; lines of the width of the record before, of a new width
 *        or of random lengths (also 0), empty lines after a record, the line end \n or \r\n, sometimes lines before the
 *        first header line and no line end after the last line
 * @tparam gen_t random number generator type
 * @param max_size the largest length of the sequences
 * @param max_sigma the largest number of distinct bytes of the sequences and the headers (at most 255)
 * @param gen random number generator
 * @return the file
 */
template <typename gen_t>
inline std::string random_fasta_file(uint64_t max_size, uint64_t max_sigma, gen_t& gen)
{
    auto coin = [&] { return std::bernoulli_distribution()(gen); };
    auto uniform = [&](uint64_t min, uint64_t max) { return std::uniform_int_distribution<uint64_t>(min, max)(gen); };
    const std::string line_end = coin() ? "\r\n" : "\n";

    // the alphabet: distinct bytes other than the line feed
    std::vector<uint8_t> alphabet(num_byte_values);
    std::iota(alphabet.begin(), alphabet.end(), uint8_t(0));
    alphabet.erase(alphabet.begin() + fasta_line_feed);
    std::shuffle(alphabet.begin(), alphabet.end(), gen);
    alphabet.resize(uniform(1, std::min<uint64_t>(max_sigma, alphabet.size())));
    auto symbol = [&] { return char(alphabet[uniform(0, alphabet.size() - 1)]); };

    // the sequences: a repetitive sequence with runs of one byte and lowercase runs
    const uint64_t n = random_log_uniform_size(1, max_size + 1, gen) - 1;
    std::string sequences(n, '\0');

    if (n > 0) {
        const std::vector<uint8_t> symbols = generate_repetitive_input<std::vector<uint8_t>>(
            random_repetitive_params(n, gen), 0, uint8_t(alphabet.size() - 1));
        for (uint64_t i = 0; i < n; i++) sequences[i] = char(alphabet[symbols[i]]);
    }

    for (uint64_t runs = uniform(0, std::bit_width(n)); runs > 0; runs--) {
        const uint64_t first = uniform(0, n - 1);
        const uint64_t length = random_log_uniform_size(1, n - first, gen);
        const bool lowercase = coin(); // a lowercase run or a run of one byte
        const char masked = symbol();

        for (uint64_t i = first; i < first + length; i++)
            sequences[i] = lowercase ? char(std::tolower((unsigned char) sequences[i])) : masked;
    }

    // the sequences of the records: [cuts[i], cuts[i + 1])
    const uint64_t num_records = random_log_uniform_size(1, n + 1, gen);
    std::vector<uint64_t> cuts { 0, n };
    for (uint64_t i = 1; i < num_records; i++) cuts.push_back(uniform(0, n));
    std::sort(cuts.begin(), cuts.end());

    std::string file;
    std::string header; // the header of the record before
    std::string prefix; // the fixed prefix of the headers
    for (uint64_t i = uniform(0, std::bit_width(n)); i > 0; i--) prefix += symbol();
    uint64_t width = random_log_uniform_size(1, n + 1, gen); // the width of the lines of the record before
    const bool headerless = coin(); // whether the lines of the first record are before the first header line

    for (uint64_t r = 0; r + 1 < cuts.size(); r++) {
        if (r > 0 || !headerless) {
            const uint64_t kind = uniform(0, 2);

            if (kind == 0) {
                header = prefix + std::to_string(r);
            } else if (kind == 1 && !header.empty()) {
                header[uniform(0, header.size() - 1)] = symbol();
            } else {
                header.clear();
                for (uint64_t i = uniform(0, std::bit_width(n)); i > 0; i--) header += symbol();
            }

            file += char(fasta_header_marker) + header + line_end;
        }

        const uint64_t layout = uniform(0, 2); // 0: the width before, 1: a new width, 2: random line lengths
        if (layout == 1) width = random_log_uniform_size(1, n + 1, gen);

        for (uint64_t i = cuts[r]; i < cuts[r + 1];) {
            const uint64_t length = std::min(cuts[r + 1] - i, layout == 2 ? uniform(0, 2 * width) : width);
            file += sequences.substr(i, length) + line_end;
            i += length;
        }

        while (coin()) file += line_end;
    }

    if (coin() && file.ends_with(line_end)) file.resize(file.size() - line_end.size());
    return file;
}

/**
 * @brief draws the alphabet of a fasta_transform: fasta_default_alphabet or, with probability 1/2, random bytes (see
 *        random_byte_alphabet) other than fasta_default_separator
 * @tparam gen_t random number generator type
 * @param gen random number generator
 */
template <typename gen_t>
inline std::string random_fasta_alphabet(gen_t& gen)
{
    if (std::bernoulli_distribution()(gen)) return std::string(fasta_default_alphabet);
    std::string alphabet;
    for (uint8_t c : random_byte_alphabet(num_byte_values, gen)) if (c != fasta_default_separator) alphabet.push_back(char(c));
    return alphabet;
}

/**
 * @brief returns the text of a FASTA file: the transformation applied to its bytes one after another
 * @param file the FASTA file
 * @param transform the transformation
 */
inline std::string reference_fasta_text(const std::string& file, const fasta_transform& transform)
{
    std::string text;
    fasta_transform::state_t state;

    for (uint64_t i = 0; i < file.size(); i++) {
        const bool line_feed_follows = i + 1 < file.size() && uint8_t(file[i + 1]) == fasta_line_feed;
        uint8_t c = 0;
        const fasta_transform::step_t step = transform.step(uint8_t(file[i]), state, c, [&] { return line_feed_follows; });
        if (step == fasta_transform::_symbol) text.push_back(char(c));
        if (step == fasta_transform::_separator) text.push_back(char(transform.separator));
    }

    return text;
}

/**
 * @brief checks the FASTA mode on a file: splits it (see split_fasta), builds a parsing of its sequence text, writes
 *        the file and random ranges of it (see write_fasta) and compares them with the bytes of the file; checks that
 *        the positions of fasta_position_map lead to the same bytes in the file and in the sequence text
 * @tparam build_fnc_t type of build
 * @tparam gen_t random number generator type
 * @param file the file
 * @param p the number of threads
 * @param build build(T) returns a parsing of the sequence text T (with |T| > 0)
 * @param gen random number generator
 */
template <typename build_fnc_t, typename gen_t>
inline void verify_fasta_file(const std::string& file, uint64_t p, build_fnc_t build, gen_t& gen)
{
    auto open = [&] { return memory_file(reinterpret_cast<const uint8_t*>(file.data()), file.size()); };
    const fasta_parts parts = split_fasta(open, p, false, random_log_uniform_size(1, file.size() + 1, gen));
    std::vector<uint8_t> bytes;
    parts.section.write(bytes);
    byte_reader reader(bytes.data(), bytes.size());
    const fasta_section section = fasta_section::read(reader);
    ASSERT_EQ(reader.remaining(), uint64_t(0));

    const uint64_t n = parts.text.text_size;
    std::string T(n, '\0');

    if (n > 0) {
        fasta_text_reader<memory_file>(open(), parts.transform, parts.text.samples.data(), parts.text.samples.size())
            .read(0, n, reinterpret_cast<uint8_t*>(T.data()));
    }

    const auto parsing = n > 0 ? build(T) : decltype(build(T))();
    phase_progress progress(false, 0);

    std::string restored;
    fasta_output restored_out(&restored);
    write_fasta(parsing, n, section, nullptr, {}, restored_out, "", p, progress);
    restored_out.finish();
    ASSERT_EQ(restored, file);

    const fasta_position_map map(section);
    ASSERT_EQ(map.file_size(), file.size());
    ASSERT_EQ(map.text_size(), n);
    ASSERT_EQ(map.file_position(n), file.size());

    for (uint64_t i = 0; i < n; i++) {
        const uint64_t pos = map.file_position(i);
        ASSERT_EQ(map.text_position(pos), i);
        ASSERT_EQ(file[pos], T[i] == char(fasta_line_feed) ? char(fasta_header_marker) : T[i]) << i;
    }

    for (uint64_t pos = 0; pos <= file.size(); pos++) {
        const uint64_t i = map.text_position(pos);
        ASSERT_GE(map.file_position(i), pos);
        if (i > 0) { ASSERT_LT(map.file_position(i - 1), pos); }
    }

    if (file.empty()) return;
    std::vector<extract_range_t> ranges(random_log_uniform_size(1, std::bit_width(file.size()), gen));
    std::string expected;

    for (auto& [position, length] : ranges) {
        position = std::uniform_int_distribution<uint64_t>(0, file.size() - 1)(gen);
        length = random_log_uniform_size(1, file.size() - position, gen);
        expected += file.substr(position, length);
    }

    std::string extracted;
    fasta_output extracted_out(&extracted);
    write_fasta(parsing, n, section, &map, ranges, extracted_out, "", p, progress);
    extracted_out.finish();
    ASSERT_EQ(extracted, expected);
}

#endif // STRING_UTILS_TESTS_TEST_FASTA_HPP
