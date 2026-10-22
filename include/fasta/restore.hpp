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
 * @file restore.hpp
 * @brief restores a FASTA file from its parts (see split.hpp)
 */

#ifndef STRING_UTILS_FASTA_RESTORE_HPP
#define STRING_UTILS_FASTA_RESTORE_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/range_coder.hpp>
#include <fasta/header_model.hpp>
#include <fasta/layout_model.hpp>
#include <fasta/scan.hpp>
#include <fasta/split.hpp>
#include <misc/files.hpp>
#include <misc/memory.hpp>

/**
 * @brief writes bytes to a file (with O_DIRECT, created at the first write) or to a string through a buffer
 */
class fasta_output {
private:
    std::string path; // the path of the file (empty: the bytes are appended to the string)
    std::unique_ptr<direct_ofstream> file; // the file
    std::string* str = nullptr; // the string
    std::vector<char> buffer; // the bytes that have not been written to the file
    uint64_t written = 0; // the number of bytes written to the file

    // writes len bytes to the file
    void write(const char* bytes, uint64_t len)
    {
        if (!file) {
            file = std::make_unique<direct_ofstream>(path);
            if (!file->is_open()) throw std::runtime_error("cannot create " + path);
        }

        write_in_chunks(*file, bytes, len);
        if (!file->good()) throw std::runtime_error("cannot write the decompressed text");
        written += len;
    }

    // writes the buffered bytes to the file
    void flush()
    {
        write(buffer.data(), buffer.size());
        buffer.clear();
    }

public:
    // writes to the file at path
    explicit fasta_output(const std::string& path) : path(path) { buffer.reserve(huge_page_bytes); }

    // appends to the string *str
    explicit fasta_output(std::string* str) : str(str) { }

    // appends bytes
    void append(const char* bytes, uint64_t len)
    {
        if (path.empty()) {
            str->append(bytes, len);
            return;
        }

        if (buffer.size() + len > huge_page_bytes) flush();

        if (len > huge_page_bytes) {
            write(bytes, len);
        } else {
            buffer.insert(buffer.end(), bytes, bytes + len);
        }
    }

    // removes the last len bytes; they must have been appended after the last append of more than huge_page_bytes
    void remove_last(uint64_t len)
    {
        if (path.empty()) {
            if (str->size() < len) throw invalid_file();
            str->resize(str->size() - len);
        } else {
            if (buffer.size() < len) throw invalid_file();
            buffer.resize(buffer.size() - len);
        }
    }

    // writes the remaining bytes and returns the number of bytes of the output
    uint64_t finish()
    {
        if (path.empty()) return str->size();
        flush();
        file->close();
        if (!file->good()) throw std::runtime_error("cannot write " + path);
        return written;
    }
};

/**
 * @brief restores a FASTA file from its parts; the sequence text is passed in consecutive pieces (see append), and
 *        each piece is written as soon as it is passed
 */
class fasta_restorer {
private:
    static constexpr char header_marker = char(fasta_header_marker); // the first byte of a header line
    static constexpr char line_feed = char(fasta_line_feed); // the separator of the records in the sequence text
    static constexpr char carriage_return = char(fasta_carriage_return); // the byte before \n in a \r\n line end

    fasta_output& out; // the output
    uint8_t flags; // the flags
    uint64_t num_records; // the number of records
    range_decoder headers; // decodes the headers
    range_decoder layout; // decodes the line layout
    fasta_header_model header_coder; // the model of the headers
    fasta_layout_model layout_coder; // the model of the line layout
    std::string line_end; // the line end
    uint64_t record = 0; // the current record
    bool started = false; // whether the header line of the current record has been written
    bool decoded = false; // whether the start of the lines of the current record has been decoded
    uint64_t width = 0; // W, if the lines of the current record are regular and L > 0 (else 0)
    uint64_t runs_left = 0; // the number of runs of the current record that have not been decoded
    uint64_t run_length = 0; // the line length of the current run
    uint64_t lines_left = 0; // the number of lines of the current run that have not been written
    uint64_t column = 0; // the number of bytes of the current line that have been written

    // writes count line ends
    void write_line_ends(uint64_t count)
    {
        for (uint64_t i = 0; i < count; i++) out.append(line_end.data(), line_end.size());
    }

    // starts the next record: writes its header line, if it has one
    void start_record()
    {
        if (record >= num_records) throw invalid_file();

        if (record > 0 || !(flags & fasta_headerless_first_record)) {
            const std::string header = header_coder.decode(headers);
            out.append(&header_marker, 1);
            out.append(header.data(), header.size());
            write_line_ends(1);
        }

        started = true;
        decoded = false;
    }

    // decodes the start of the lines of the current record (nonempty: whether L > 0)
    void decode_lines(bool nonempty)
    {
        width = layout_coder.decode_start(layout, nonempty, runs_left);
        lines_left = 0;
        column = 0;
        decoded = true;
    }

    // writes bytes of the sequence of the current record (without a separator)
    void write_sequence(const char* bytes, uint64_t len)
    {
        while (len > 0) {
            if (width == 0 && lines_left == 0) {
                if (runs_left == 0) throw invalid_file();
                std::tie(run_length, lines_left) = layout_coder.decode_run(layout);
                runs_left--;
            }

            const uint64_t line_length = width != 0 ? width : run_length;

            if (line_length == 0) {
                write_line_ends(lines_left);
                lines_left = 0;
                continue;
            }

            const uint64_t copied = std::min(len, line_length - column);
            out.append(bytes, copied);
            bytes += copied;
            len -= copied;
            column += copied;

            if (column == line_length) {
                write_line_ends(1);
                column = 0;
                if (width == 0) lines_left--;
            }
        }
    }

    // ends the current record: its last line, its remaining runs of empty lines and its empty lines at the end
    void end_record()
    {
        if (width != 0) {
            if (column > 0) write_line_ends(1);
        } else {
            if (column > 0 || (lines_left > 0 && run_length > 0)) throw invalid_file();
            write_line_ends(lines_left);

            for (; runs_left > 0; runs_left--) {
                const auto [length, count] = layout_coder.decode_run(layout);
                if (length > 0) throw invalid_file();
                write_line_ends(count);
            }
        }

        write_line_ends(layout_coder.decode_empty_lines(layout));
        lines_left = column = width = 0;
        record++;
        started = false;
    }

public:
    /**
     * @brief starts restoring a FASTA file
     * @param section the FASTA section (it must stay valid while the restorer is used)
     * @param out the output
     */
    fasta_restorer(const fasta_section& section, fasta_output& out)
        : out(out), flags(section.flags), num_records(section.num_records),
          headers(section.headers.data(), section.headers.size()), layout(section.layout.data(), section.layout.size()),
          line_end(flags & fasta_crlf ? std::string { carriage_return, line_feed } : std::string(1, line_feed)) { }

    /**
     * @brief writes the next bytes of the sequence text with the headers and the line ends between them
     * @param T the bytes
     * @param len their number
     */
    void append(const char* T, uint64_t len)
    {
        uint64_t pos = 0;

        while (pos < len) {
            if (!started) start_record();
            if (!decoded) decode_lines(T[pos] != line_feed);

            if (T[pos] == line_feed) {
                end_record();
                pos++;
                continue;
            }

            const char* separator = static_cast<const char*>(std::memchr(T + pos, line_feed, len - pos));
            const uint64_t end = separator == nullptr ? len : uint64_t(separator - T);
            write_sequence(T + pos, end - pos);
            pos = end;
        }
    }

    // ends the last record after the whole sequence text has been passed
    void finish()
    {
        if (num_records == 0) {
            if (flags != 0) throw invalid_file();
            return;
        }

        if (!started) start_record();
        if (!decoded) decode_lines(false);
        end_record();
        if (record != num_records) throw invalid_file();
        if (flags & fasta_missing_final_line_end) out.remove_last(line_end.size());
    }
};

#endif // STRING_UTILS_FASTA_RESTORE_HPP
