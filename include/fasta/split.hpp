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
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/range_coder.hpp>
#include <fasta/header_model.hpp>
#include <fasta/layout_model.hpp>
#include <fasta/scan.hpp>
#include <misc/files.hpp>
#include <misc/log.hpp>
#include <misc/memory.hpp>
#include <misc/threads.hpp>

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
    void write(std::vector<uint8_t>& out) const;

    /**
     * @brief reads a section written by write
     * @param in a byte_reader or a stream_byte_reader
     */
    template <typename reader_t>
    static fasta_section read(reader_t& in);
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
inline fasta_transform fasta_sequence_transform(bool crlf);

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
    void add_line(uint64_t length);

    void header_start() { header_begin = headers.size(); }
    void header_bytes(const uint8_t* bytes, uint64_t len) { headers.append(reinterpret_cast<const char*>(bytes), len); }
    void sequence_symbols(uint64_t count) { line_length += count; }

    void line_end(bool header, bool after_cr);

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
    phase_progress& progress, uint64_t& line_feeds, uint64_t& crlf_line_ends);

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
    uint64_t min_chunk_bytes = fasta_min_chunk_bytes);

/**
 * @brief reads ranges of the sequence text of a FASTA file (see split_fasta) block by block; each thread has its own
 */
class alignas(cache_line_bytes) fasta_text_block_reader {
private:
    std::optional<fasta_text_reader<direct_reader>> reader; // the reader of the sequence text
    no_init_vector<uint8_t> buffer; // the current block

public:
    // returns whether the FASTA file is open
    bool is_open() const { return reader.has_value(); }

    /**
     * @brief opens the FASTA file
     * @param path the path of the FASTA file
     * @param parts its parts (they must stay valid while the reader is used)
     * @param block the number of symbols of a block
     */
    void open(const std::string& path, const fasta_parts& parts, uint64_t block)
    {
        reader.emplace(direct_reader(path), parts.transform, parts.text.samples.data(), parts.text.samples.size());
        resize_no_init(buffer, block);
    }

    /**
     * @brief reads the symbols [first, to) of the sequence text block by block
     * @param first the first position
     * @param to the position after the last one
     * @param process called as process(symbols, length) with each block
     */
    template <typename process_fnc_t>
    void for_each_block(uint64_t first, uint64_t to, process_fnc_t process);
};

/**
 * @brief writes the sequence text of a FASTA file (see split_fasta) to a file; each thread writes a range of it
 * @param path_fasta the path of the FASTA file
 * @param parts its parts
 * @param path the path of the file the sequence text is written to
 * @param num_threads the number of threads
 */
inline void write_fasta_text(const std::string& path_fasta, const fasta_parts& parts, const std::string& path,
    uint64_t num_threads);

#include <fasta/split.tpp>

#endif // STRING_UTILS_FASTA_SPLIT_HPP
