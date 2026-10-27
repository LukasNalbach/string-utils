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
 * @file byte_file.hpp
 * @brief the text of a byte file or of a FASTA file as the input of a construction (build_byte_file)
 */

#ifndef STRING_UTILS_RLBWT_BYTE_FILE_HPP
#define STRING_UTILS_RLBWT_BYTE_FILE_HPP

#include <cstdint>
#include <string>
#include <type_traits>

#include <fasta/scan.hpp>
#include <misc/files.hpp>
#include <misc/log.hpp>
#include <rlbwt/byte_map.hpp>
#include <rlbwt/fasta_source.hpp>
#include <rlbwt/text_sources.hpp>

namespace rlbwt {

/**
 * @brief calls build(text) to build the output of the text of a file: the file or, if fasta is not nullptr, the text
 *        of the transformation *fasta of the file (see fasta_source), optionally reversed; text is a text_source of
 *        uint8_t or of uint16_t that maps the bytes of the text if one is below min_symbol (see build_with_byte_map).
 *        Before, the file is scanned for the bytes of its text.
 * @param path the file
 * @param fasta the transformation of a FASTA file (nullptr: the text is the file)
 * @param reverse whether the text is read reversed
 * @param min_symbol the smallest symbol the construction accepts
 * @param prm the parameters of the construction (see build_with_byte_map)
 * @param num_threads the number of threads of the scan
 * @param log whether the scan is logged
 * @param build builds the output of a text
 * @return the result of build
 */
template <typename build_t>
auto build_byte_file(const std::string& path, const fasta_transform* fasta, bool reverse, uint64_t min_symbol,
    output_params& prm, uint64_t num_threads, bool log, build_t build)
{
    auto time = now();

    if (fasta == nullptr) {
        log_phase_start(log, time, "reading the alphabet");
        const file_source<uint8_t> bytes(path, reverse);
        phase_progress progress(log, bytes.size());
        const byte_set occurs = occurring_bytes(file_source<uint8_t>(path), num_threads, &progress);
        progress.finish();
        log_phase_end(log, time);

        return build_with_byte_map(occurs, min_symbol, prm, [&](const auto* map) {
            if (map == nullptr) return build(bytes);
            return build(mapped_byte_source(bytes, *map));
        });
    }

    log_phase_start(log, time, "scanning the FASTA file");
    phase_progress progress(log, direct_reader(path).size());
    const fasta_scan scan = scan_fasta_lines<fasta_chunk>([&] { return direct_reader(path); }, *fasta,
        { .num_threads = num_threads, .progress = &progress }, [](auto&&...) { });
    progress.finish();
    log_phase_end(log, time);
    byte_set occurs;
    for (uint64_t c = 0; c < num_byte_values; c++) occurs[c] = scan.freq[c] != 0;

    return build_with_byte_map(occurs, min_symbol, prm, [&](const auto* map) {
        using sym_t = typename std::remove_cvref_t<decltype(*map)>::value_type;
        return build(fasta_source<sym_t>(path, scan.text_size, *fasta, scan.samples.data(), scan.samples.size(),
            reverse, map, num_threads));
    });
}

} // namespace rlbwt

#endif // STRING_UTILS_RLBWT_BYTE_FILE_HPP
