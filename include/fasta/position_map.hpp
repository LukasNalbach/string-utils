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
 * @file position_map.hpp
 * @brief the map of the positions of a FASTA file to the positions of its sequence text (see split.hpp) and back, and
 *        the restoration of ranges of the file from the ranges of the sequence text they cover
 */

#ifndef STRING_UTILS_FASTA_POSITION_MAP_HPP
#define STRING_UTILS_FASTA_POSITION_MAP_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/range_coder.hpp>
#include <fasta/header_model.hpp>
#include <fasta/layout_model.hpp>
#include <fasta/restore.hpp>
#include <fasta/scan.hpp>
#include <fasta/split.hpp>

// a range of a FASTA file or of its sequence text: (position, length)
using fasta_range = std::pair<uint64_t, uint64_t>;

class fasta_range_writer;

/**
 * @brief maps the positions of a FASTA file to the positions of its sequence text and back; built from the FASTA
 *        section. The file consists of pieces: a header line, or consecutive sequence lines of one length l, each of
 *        them l symbols of the sequence text followed by the line end. The line feed between the sequences of the
 *        records i - 1 and i in the sequence text is at the first byte of the header line of record i.
 */
class fasta_position_map {
    friend class fasta_range_writer;

public:
    // a header line or consecutive sequence lines of one length
    struct piece {
        uint64_t file_start; // the position of its first byte in the file
        uint64_t text_start; // the position of its first symbol in the sequence text (without symbols: of the next one)
        // sequence lines: the length of a line without its line end; a header line: the start of its bytes in
        // header_lines
        uint64_t value;
        bool header; // whether it is a header line
    };

private:
    std::string line_end; // the line end
    std::string header_lines; // the header lines with fasta_header_marker and their line ends, in their order
    std::vector<piece> pieces; // the pieces in their order, then one at the end of the file with every line end
    uint64_t size = 0; // the size of the file in bytes

    // returns the index of the piece that contains a position of the file
    uint64_t piece_at(uint64_t pos) const
    {
        return uint64_t(std::upper_bound(pieces.begin(), pieces.end() - 1, pos,
            [](uint64_t p, const piece& x) { return p < x.file_start; }) - pieces.begin()) - 1;
    }

public:
    /**
     * @brief decodes the headers and the line layout of a FASTA file
     * @param section the FASTA section
     */
    explicit fasta_position_map(const fasta_section& section)
        : line_end(section.flags & fasta_crlf ? std::string { char(fasta_carriage_return), char(fasta_line_feed) }
              : std::string(1, char(fasta_line_feed)))
    {
        if (section.num_records == 0 && section.flags != 0) throw invalid_file();
        range_decoder headers(section.headers.data(), section.headers.size());
        range_decoder layout(section.layout.data(), section.layout.size());
        fasta_header_model header_coder;
        fasta_layout_model layout_coder;
        uint64_t file_pos = 0; // the start of the next piece in the file
        uint64_t text_pos = 0; // the start of the next piece in the sequence text

        // appends count sequence lines of a length
        auto add_lines = [&](uint64_t length, uint64_t count) {
            if (count == 0) return;
            pieces.push_back({ file_pos, text_pos, length, false });
            file_pos += count * (length + line_end.size());
            text_pos += count * length;
        };

        for (uint64_t record = 0; record < section.num_records; record++) {
            if (record > 0 || !(section.flags & fasta_headerless_first_record)) {
                const std::string header = header_coder.decode(headers);
                pieces.push_back({ file_pos, text_pos, header_lines.size(), true });
                header_lines += char(fasta_header_marker);
                header_lines += header;
                header_lines += line_end;
                file_pos += 1 + header.size() + line_end.size();
            }

            if (record > 0) text_pos++;
            uint64_t L, runs;
            const uint64_t width = layout_coder.decode_start(layout, L, runs);

            if (width != 0) {
                const uint64_t lines = (L - 1) / width + 1; // the number of lines
                const uint64_t last = L - (lines - 1) * width; // the length of the last line

                if (last == width) {
                    add_lines(width, lines);
                } else {
                    add_lines(width, lines - 1);
                    add_lines(last, 1);
                }
            }

            for (; runs > 0; runs--) {
                const auto [length, count] = layout_coder.decode_run(layout);
                add_lines(length, count);
            }

            add_lines(0, layout_coder.decode_empty_lines(layout));
        }

        pieces.push_back({ file_pos, text_pos, 0, false });
        size = file_pos;

        if (section.flags & fasta_missing_final_line_end) {
            if (size < line_end.size()) throw invalid_file();
            size -= line_end.size();
        }
    }

    // returns the size of the file in bytes
    uint64_t file_size() const { return size; }

    // returns the length of the sequence text
    uint64_t text_size() const { return pieces.back().text_start; }

    /**
     * @brief returns the number of symbols of the sequence text before a position of the file, i.e., the position in
     *        the sequence text of the first symbol at or after it
     * @param pos [0..file_size()] the position in the file
     */
    uint64_t text_position(uint64_t pos) const
    {
        if (pos >= size) return text_size();
        const uint64_t k = piece_at(pos);
        const piece& x = pieces[k];
        const uint64_t offset = pos - x.file_start;
        if (x.header) return offset == 0 ? x.text_start : pieces[k + 1].text_start;
        const uint64_t line = x.value + line_end.size(); // the bytes of a line with its line end
        return x.text_start + offset / line * x.value + std::min(offset % line, x.value);
    }

    /**
     * @brief returns the position in the file of a symbol of the sequence text (of a line feed between two records:
     *        the start of the header line of the second one)
     * @param pos [0..text_size()] the position in the sequence text (text_size(): file_size() is returned)
     */
    uint64_t file_position(uint64_t pos) const
    {
        if (pos >= text_size()) return size;
        const piece& x = *(std::upper_bound(pieces.begin(), pieces.end(), pos,
            [](uint64_t p, const piece& y) { return p < y.text_start; }) - 1);
        if (x.header) return x.file_start;
        const uint64_t offset = pos - x.text_start;
        return x.file_start + offset / x.value * (x.value + line_end.size()) + offset % x.value;
    }

    /**
     * @brief returns the ranges of the sequence text that ranges of the file cover, without the empty ones
     * @param ranges the ranges of the file (position, length), with position + length <= file_size()
     * @return the ranges of the sequence text (position, length), in the order of the ranges of the file
     */
    std::vector<fasta_range> text_ranges(const std::vector<fasta_range>& ranges) const
    {
        std::vector<fasta_range> covered;

        for (const auto& [position, length] : ranges) {
            const uint64_t first = text_position(position);
            const uint64_t to = text_position(position + length);
            if (first < to) covered.emplace_back(first, to - first);
        }

        return covered;
    }
};

/**
 * @brief writes ranges of a FASTA file: their header lines and line ends from a fasta_position_map, their sequence
 *        bytes from the symbols of the sequence text they cover (see fasta_position_map::text_ranges), which are passed
 *        in consecutive pieces (see append)
 */
class fasta_range_writer {
private:
    static constexpr char line_feed = char(fasta_line_feed); // the separator of the records in the sequence text

    const fasta_position_map& map; // the map of the file
    fasta_output& out; // the output
    const std::vector<fasta_range>& ranges; // the ranges of the file (position, length)
    uint64_t range = 0; // the current range
    uint64_t pos = 0; // the next position of the file to write
    uint64_t end = 0; // the end of the current range
    uint64_t k = 0; // the piece that contains pos

    // starts the current range, if there is one
    void start_range()
    {
        if (range == ranges.size()) return;
        pos = ranges[range].first;
        end = pos + ranges[range].second;
        k = map.piece_at(pos);
    }

    // writes the bytes from pos to the next sequence byte of the ranges, or to the end of the last range
    void write_literals()
    {
        while (range < ranges.size()) {
            if (pos == end) {
                range++;
                start_range();
                continue;
            }

            while (pos >= map.pieces[k + 1].file_start) k++;
            const fasta_position_map::piece& x = map.pieces[k];

            if (x.header) {
                const uint64_t len = std::min(end, map.pieces[k + 1].file_start) - pos;
                out.append(map.header_lines.data() + x.value + (pos - x.file_start), len);
                pos += len;
                continue;
            }

            const uint64_t line = x.value + map.line_end.size(); // the bytes of a line with its line end
            const uint64_t column = (pos - x.file_start) % line;
            if (column < x.value) return;
            const uint64_t len = std::min(line - column, end - pos);
            out.append(map.line_end.data() + (column - x.value), len);
            pos += len;
        }
    }

public:
    /**
     * @brief starts writing ranges of a FASTA file
     * @param map the map of the file
     * @param out the output
     * @param ranges the ranges of the file (position, length), with 1 <= length <= map.file_size() - position (they
     *        must stay valid while the writer is used)
     */
    fasta_range_writer(const fasta_position_map& map, fasta_output& out, const std::vector<fasta_range>& ranges)
        : map(map), out(out), ranges(ranges) { start_range(); }

    /**
     * @brief writes the next symbols of the sequence text that the ranges cover, with the headers and the line ends
     *        between them
     * @param T the symbols
     * @param len their number
     */
    void append(const char* T, uint64_t len)
    {
        const char* const T_end = T + len;

        while (T < T_end) {
            if (*T == line_feed) {
                T++;
                continue;
            }

            write_literals();
            if (range == ranges.size()) throw invalid_file();
            const fasta_position_map::piece& x = map.pieces[k];
            const uint64_t column = (pos - x.file_start) % (x.value + map.line_end.size());
            uint64_t copied = std::min({ x.value - column, end - pos, uint64_t(T_end - T) });
            const char* separator = static_cast<const char*>(std::memchr(T, line_feed, copied));
            if (separator != nullptr) copied = uint64_t(separator - T);
            out.append(T, copied);
            T += copied;
            pos += copied;
        }
    }

    // writes the rest of the ranges after all symbols have been passed
    void finish()
    {
        write_literals();
        if (range < ranges.size()) throw invalid_file();
    }
};

#endif // STRING_UTILS_FASTA_POSITION_MAP_HPP
