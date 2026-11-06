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

#ifndef STRING_UTILS_FASTA_SPLIT_TPP
#define STRING_UTILS_FASTA_SPLIT_TPP

#include <fasta/split.hpp>

inline void fasta_section::write(std::vector<uint8_t>& out) const
{
    out.push_back(flags);
    write_varint(out, num_records);
    write_varint(out, headers.size());
    write_varint(out, layout.size());
    out.insert(out.end(), headers.begin(), headers.end());
    out.insert(out.end(), layout.begin(), layout.end());
}

template <typename reader_t>
inline fasta_section fasta_section::read(reader_t& in)
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

inline void fasta_lines_chunk::add_line(uint64_t length)
{
    if (lines.size() > (continues() ? 1 : 0) && lines.back().second != 0 && lines.back().first == length) {
        lines.back().second++;
    } else {
        lines.emplace_back(length, 1);
    }
}

inline void fasta_lines_chunk::line_end(bool header, bool after_cr)
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

template <typename open_fnc_t>
inline fasta_parts split_fasta(open_fnc_t open, uint64_t num_threads, bool log, uint64_t min_chunk_bytes)
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

template <typename process_fnc_t>
inline void fasta_text_block_reader::for_each_block(uint64_t first, uint64_t to, process_fnc_t process)
{
    for (uint64_t pos = first; pos < to; pos += buffer.size()) {
        const uint64_t len = std::min<uint64_t>(buffer.size(), to - pos);
        reader->read(pos, len, buffer.data());
        process(buffer.data(), len);
    }
}

inline void write_fasta_text(const std::string& path_fasta, const fasta_parts& parts, const std::string& path, uint64_t num_threads)
{
    const uint64_t n = parts.text.text_size;
    direct_positioned_file out(path, n);

    run_on_threads(num_threads, [&](uint64_t t, uint64_t p) {
        const uint64_t first = n * t / p;
        const uint64_t to = n * (t + 1) / p;
        if (first >= to) return;
        fasta_text_block_reader reader;
        reader.open(path_fasta, parts, std::min(to - first, huge_page_bytes));
        uint64_t pos = first;

        reader.for_each_block(first, to, [&](const uint8_t* symbols, uint64_t len) {
            out.write(reinterpret_cast<const char*>(symbols), len, pos);
            pos += len;
        });
    });

    if (!out.close_file()) throw std::runtime_error("cannot write " + path);
}

#endif // STRING_UTILS_FASTA_SPLIT_TPP
