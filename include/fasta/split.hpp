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
 * @file split.hpp
 * @brief splits a FASTA file into its parts: the header lines and the line layout, coded with context models, and the
 *        sequence text, which is read from the FASTA file (see fasta_text_reader); restore.hpp restores the file
 */

#ifndef STRING_UTILS_FASTA_SPLIT_HPP
#define STRING_UTILS_FASTA_SPLIT_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/range_coder.hpp>
#include <fasta/header_model.hpp>
#include <fasta/layout_model.hpp>
#include <fasta/scan.hpp>
#include <misc/log.hpp>

// the first byte of a compressed file of a compressor with a FASTA mode: the text is stored as it is, or as a FASTA
// file split into its parts (see fasta_section)
inline constexpr uint8_t text_mode_plain = 0;
inline constexpr uint8_t text_mode_fasta = 1;

// flags of the FASTA section of a compressed file
inline constexpr uint8_t fasta_crlf = 1; // every line feed is preceded by a carriage return (part of the line end)
inline constexpr uint8_t fasta_missing_final_line_end = 2; // the last line has no line end
inline constexpr uint8_t fasta_headerless_first_record = 4; // the file does not start with a header line

/**
 * @brief the FASTA section of a compressed file: the flags (one byte), the number of records, the size of the coded
 *        headers and the size of the coded line layout in bytes (variable-length integers, see write_varint), then the
 *        coded headers and the coded line layout
 */
struct fasta_section {
    uint8_t flags = 0; // the flags (fasta_crlf, ...)
    uint64_t num_records = 0; // the number of records
    std::vector<uint8_t> headers; // the coded headers
    std::vector<uint8_t> layout; // the coded line layout

    // appends the section to bytes
    void write(std::vector<uint8_t>& out) const
    {
        out.push_back(flags);
        write_varint(out, num_records);
        write_varint(out, headers.size());
        write_varint(out, layout.size());
        out.insert(out.end(), headers.begin(), headers.end());
        out.insert(out.end(), layout.begin(), layout.end());
    }

    /**
     * @brief reads a section written by write
     * @param in a byte_reader or a stream_byte_reader
     */
    template <typename reader_t>
    static fasta_section read(reader_t& in)
    {
        fasta_section section;
        section.flags = in.byte();
        section.num_records = in.varint();
        const uint64_t header_size = in.varint();
        const uint64_t layout_size = in.varint();
        const uint8_t* headers = in.bytes(header_size);
        section.headers.assign(headers, headers + header_size);
        const uint8_t* layout = in.bytes(layout_size);
        section.layout.assign(layout, layout + layout_size);
        return section;
    }
};

/**
 * @brief the parts of a FASTA file: the coded headers, the coded line layout and the sequence text, which is read
 *        from the FASTA file with the transformation (see fasta_sequence_transform)
 */
struct fasta_parts {
    fasta_section section; // the flags, the number of records, the coded headers and the coded line layout
    fasta_transform transform; // the transformation that yields the sequence text
    fasta_scan text; // the length, the byte frequencies and the samples of the sequence text
};

/**
 * @brief returns the transformation that yields the sequence text of a FASTA file: the bytes of the sequence lines
 *        of each record without their line ends, and a line feed between the sequences of consecutive records
 * @param crlf whether every line feed is preceded by a carriage return that belongs to the line end
 */
inline fasta_transform fasta_sequence_transform(bool crlf)
{
    fasta_transform transform;
    transform.in_alphabet.fill(1);
    transform.separator = fasta_line_feed;
    transform.to_upper = false;
    transform.separator_before_first_record = false;
    transform.lines_before_first_header = true;
    transform.carriage_return = crlf ? fasta_transform::_dropped_before_line_feed : fasta_transform::_kept;
    return transform;
}

/**
 * @brief the header lines and the lengths of the sequence lines of a chunk of a FASTA file (see scan_fasta_parts); the
 *        chunk can start and end inside a line
 */
struct fasta_lines_chunk : fasta_chunk {
    /* the lines of the chunk in their order, consecutive sequence lines of equal length merged: (length, count) for
       count >= 1 sequence lines of a length, (length, 0) for a header line of a length, stored in headers; the first
       line is not merged with others if the chunk starts inside it (see continues), the last one is not if the chunk
       ends inside it (see ends_inside) */
    fasta_line_runs lines;
    std::string headers; // the bytes of the header lines of the chunk after '>' and without their line ends
    uint64_t header_begin = 0; // the start of the current header line in headers
    uint64_t line_length = 0; // the length of the current sequence line
    uint64_t line_feeds = 0; // the number of line feeds
    uint64_t crlf_line_ends = 0; // the number of line feeds directly after a carriage return

    // returns whether the chunk starts inside a line
    bool continues() const { return !start_state.at_line_start; }

    // returns whether the chunk ends inside a line
    bool ends_inside() const { return !end_state.at_line_start; }

    // appends a sequence line of a length
    void add_line(uint64_t length)
    {
        if (lines.size() > (continues() ? 1 : 0) && lines.back().second != 0 && lines.back().first == length) {
            lines.back().second++;
        } else {
            lines.emplace_back(length, 1);
        }
    }

    void header_start() { header_begin = headers.size(); }
    void header_bytes(const uint8_t* bytes, uint64_t len) { headers.append(reinterpret_cast<const char*>(bytes), len); }
    void sequence_symbols(uint64_t count) { line_length += count; }

    void line_end(bool header, bool after_cr)
    {
        line_feeds++;
        crlf_line_ends += after_cr;

        if (header) {
            lines.emplace_back(headers.size() - header_begin, 0);
        } else {
            add_line(line_length);
            line_length = 0;
        }
    }

    // appends the line the chunk ends inside, if it does
    void finish()
    {
        if (!ends_inside()) return;
        if (end_state.in_header) lines.emplace_back(headers.size() - header_begin, 0);
        else lines.emplace_back(line_length, 1);
    }
};

/**
 * @brief scans a FASTA file once, in parallel (see scan_fasta_lines): codes its headers and its line layout
 *        and computes the length, the byte frequencies and the samples of its sequence text. A line starting with
 *        fasta_header_marker is a header line, every other line is a sequence line of the current record (lines
 *        before the first header line form a record without a header).
 * @param open returns a new reader of the file (a direct_reader or a memory_file)
 * @param crlf whether every line feed is preceded by a carriage return that belongs to the line end
 * @param num_threads the number of threads
 * @param min_chunk_bytes the minimum number of bytes of a chunk of the scan
 * @param progress counts the bytes scanned
 * @param line_feeds is set to the number of line feeds of the file
 * @param crlf_line_ends is set to the number of line feeds directly after a carriage return
 * @return the parts
 */
template <typename open_fnc_t>
inline fasta_parts scan_fasta_parts(open_fnc_t open, bool crlf, uint64_t num_threads, uint64_t min_chunk_bytes,
    phase_progress& progress, uint64_t& line_feeds, uint64_t& crlf_line_ends)
{
    fasta_parts parts;
    parts.transform = fasta_sequence_transform(crlf);
    if (crlf) parts.section.flags |= fasta_crlf;
    range_encoder header_coder, layout_coder;
    fasta_header_model headers;
    fasta_layout_model layout;
    bool in_record = false; // whether a record has started
    uint64_t L = 0; // the sequence length of the current record
    fasta_line_runs lines; // the lines of the current record
    // the line that the last merged chunk ends inside: a header line (open_header) or a sequence line of a length
    bool line_open = false, open_header = false;
    uint64_t open_length = 0;
    std::string open_text; // the bytes of the open header line
    line_feeds = crlf_line_ends = 0;

    {
        auto in = open();
        const uint64_t size = in.size();

        if (size > 0 && in[0] != fasta_header_marker) {
            parts.section.flags |= fasta_headerless_first_record;
            parts.section.num_records = 1;
            in_record = true;
        }

        if (size > 0 && in[size - 1] != fasta_line_feed) parts.section.flags |= fasta_missing_final_line_end;
    }

    auto end_record = [&] {
        if (in_record) layout.encode(layout_coder, L, lines);
        L = 0;
        lines.clear();
    };

    // adds count complete lines: a header line with the bytes open_text or sequence lines of a length
    auto add_lines = [&](bool header, uint64_t length, uint64_t count) {
        if (header) {
            end_record();
            in_record = true;
            parts.section.num_records++;
            headers.encode(header_coder, open_text);
        } else {
            if (!lines.empty() && lines.back().first == length) lines.back().second += count;
            else lines.emplace_back(length, count);
            L += length * count;
        }
    };

    parts.text = scan_fasta_lines<fasta_lines_chunk>(open, parts.transform,
        { .num_threads = num_threads, .min_chunk_bytes = min_chunk_bytes, .progress = &progress },
        [&](fasta_lines_chunk& chunk, uint64_t) {
            chunk.finish();
            line_feeds += chunk.line_feeds;
            crlf_line_ends += chunk.crlf_line_ends;
            uint64_t header_pos = 0; // the start of the next header line in chunk.headers

            for (uint64_t j = 0; j < chunk.lines.size(); j++) {
                const auto [length, count] = chunk.lines[j];
                const bool header = count == 0;

                // the first line of a chunk that starts inside a line continues the open line
                if (j > 0 || !chunk.continues()) {
                    line_open = true;
                    open_header = header;
                    open_length = 0;
                    open_text.clear();
                }

                if (header) {
                    open_text.append(chunk.headers, header_pos, length);
                    header_pos += length;
                } else {
                    open_length += length;
                }

                if (j + 1 < chunk.lines.size() || !chunk.ends_inside()) {
                    add_lines(open_header, open_length, std::max<uint64_t>(1, count));
                    line_open = false;
                }
            }
        });

    if (line_open) add_lines(open_header, open_length, 1);
    end_record();
    parts.section.headers = header_coder.finish();
    parts.section.layout = layout_coder.finish();
    return parts;
}

/**
 * @brief splits a FASTA file into its parts with one scan (see scan_fasta_parts). The line ends are \r\n if every
 *        line feed is preceded by a carriage return; this is assumed if the first line feed in the first
 *        min_chunk_bytes bytes is, and the file is scanned again if the assumption is wrong.
 * @param open returns a new reader of the file (a direct_reader or a memory_file)
 * @param num_threads the number of threads
 * @param log whether to log the scan with its progress
 * @param min_chunk_bytes the minimum number of bytes of a chunk of the scan
 * @return the parts
 */
template <typename open_fnc_t>
inline fasta_parts split_fasta(open_fnc_t open, uint64_t num_threads, bool log,
    uint64_t min_chunk_bytes = fasta_min_chunk_bytes)
{
    bool crlf = false;
    uint64_t size; // the size of the file

    {
        auto in = open();
        size = in.size();
        const uint64_t searched = std::min(size, min_chunk_bytes);

        if (searched > 0) {
            const uint8_t* bytes = in.read(0, searched);
            const uint8_t* first_line_feed = static_cast<const uint8_t*>(std::memchr(bytes, fasta_line_feed, searched));
            const uint64_t i = first_line_feed == nullptr ? searched : uint64_t(first_line_feed - bytes);
            crlf = i < searched && i > 0 && bytes[i - 1] == fasta_carriage_return;
        }
    }

    auto time = now();
    log_phase_start(log, time, "scanning the FASTA file");

    for (;;) {
        phase_progress progress(log, size);
        uint64_t line_feeds, crlf_line_ends;
        fasta_parts parts = scan_fasta_parts(open, crlf, num_threads, min_chunk_bytes, progress, line_feeds,
            crlf_line_ends);
        progress.finish();
        const bool all_crlf = line_feeds > 0 && crlf_line_ends == line_feeds;

        if (all_crlf == crlf) {
            log_phase_end(log, time);
            return parts;
        }

        crlf = all_crlf;
    }
}

#endif // STRING_UTILS_FASTA_SPLIT_HPP
