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
 * @file scan.hpp
 * @brief FASTA files as texts: the transformation of the bytes of a FASTA file into the symbols of a text, the
 *        parallel scan of a FASTA file with samples of the text, a reader of the text that starts at the samples, and
 *        readers of ranges of the text that start without a scan and scan their range while they read it
 */

#ifndef STRING_UTILS_FASTA_SCAN_HPP
#define STRING_UTILS_FASTA_SCAN_HPP

#include <omp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <misc/files.hpp>
#include <misc/log.hpp>
#include <misc/memory.hpp>

// the first byte of a header line of a FASTA file
inline constexpr uint8_t fasta_header_marker = '>';

// the byte that ends a line
inline constexpr uint8_t fasta_line_feed = '\n';

// the byte before the line feed of a Windows line end
inline constexpr uint8_t fasta_carriage_return = '\r';

// number of text positions per sample of a FASTA file (see fasta_sample)
inline constexpr uint64_t fasta_sample_period = uint64_t(1) << 16;

// minimum number of bytes of a FASTA file per chunk of a parallel scan (see scan_fasta_lines)
inline constexpr uint64_t fasta_min_chunk_bytes = uint64_t(16) << 20;

/**
 * @brief maps the bytes of a FASTA file to the symbols of a text, one byte at a time (see step):
 *   - a line feed yields nothing
 *   - a carriage return yields nothing, or nothing only if a line feed follows it, or is a body byte (see
 *     carriage_return)
 *   - a header line (from '>' to the end of the line) yields one separator; if separator_before_first_record is
 *     false, the header of the first record yields nothing
 *   - a body byte that is in the alphabet (after upper-casing, if to_upper) yields itself
 *   - a run of body bytes that are not in the alphabet (a masked run) yields one separator
 *   - the lines before the first header yield nothing, or form the first record if lines_before_first_header
 */
struct fasta_transform {
    // result of processing one input byte
    enum step_t : uint8_t {
        _nothing,  // the byte yields no symbol
        _symbol,   // the byte yields itself (upper-cased, if to_upper)
        _separator // the byte yields the separator (a header or the first byte of a masked run)
    };

    // what a carriage return yields
    enum carriage_return_t : uint8_t {
        _dropped,                  // nothing
        _dropped_before_line_feed, // nothing if a line feed follows it, else it is a body byte
        _kept                      // it is a body byte
    };

    // state between two input bytes
    struct state_t {
        bool at_line_start = true;  // the next byte begins a line
        bool in_header = false;     // the current line is a header line
        bool in_sequence = false;   // a record has started
        bool in_masked_run = false; // the previous body byte was masked

        // encodes the state in the 4 lowest bits of a byte (bit 0: at_line_start, 1: in_header, 2: in_sequence,
        // 3: in_masked_run)
        uint8_t to_bits() const
        {
            return (uint8_t) ((at_line_start ? 1 : 0) | (in_header ? 2 : 0)
                | (in_sequence ? 4 : 0) | (in_masked_run ? 8 : 0));
        }

        // decodes a state encoded with to_bits
        static state_t from_bits(uint8_t bits)
        {
            return state_t {
                .at_line_start = (bits & 1) != 0,
                .in_header = (bits & 2) != 0,
                .in_sequence = (bits & 4) != 0,
                .in_masked_run = (bits & 8) != 0 };
        }
    };

    // [0..255] in_alphabet[c] = 1 <=> the byte c is in the alphabet (c is upper-cased first if to_upper)
    std::array<uint8_t, 256> in_alphabet {};
    uint8_t separator = 0; // the byte a header line and a masked run each yield
    bool to_upper = true; // whether body bytes are upper-cased
    bool separator_before_first_record = true; // whether the header of the first record yields a separator
    bool lines_before_first_header = false; // whether the lines before the first header form a record
    carriage_return_t carriage_return = _dropped; // what a carriage return yields

    fasta_transform() = default;

    /**
     * @brief builds the transformation that keeps the bytes of an alphabet, yields a separator for each header and
     *        each masked run, skips the lines before the first header and drops every carriage return
     * @param alphabet the bytes that are kept verbatim
     * @param separator the byte a header line and a masked run each yield
     * @param to_upper whether body bytes are upper-cased before they are looked up in the alphabet
     */
    fasta_transform(const std::string& alphabet, uint8_t separator, bool to_upper = true)
        : separator(separator), to_upper(to_upper)
    {
        for (uint8_t c : alphabet) in_alphabet[c] = 1;
    }

    /**
     * @brief processes one input byte
     * @param c the input byte
     * @param st the state (updated)
     * @param out (output) the yielded symbol; only set if the result is _symbol
     * @param line_feed_follows returns whether the byte after c is a line feed (only called for a carriage return
     *        with _dropped_before_line_feed)
     * @return what the byte yields
     */
    template <typename line_feed_follows_t>
    inline step_t step(uint8_t c, state_t& st, uint8_t& out, line_feed_follows_t line_feed_follows) const
    {
        if (c == fasta_carriage_return && carriage_return != _kept) {
            if (carriage_return == _dropped || line_feed_follows()) return _nothing;
        }

        if (c == fasta_line_feed) {
            if (st.at_line_start && lines_before_first_header) st.in_sequence = true;
            st.at_line_start = true;
            st.in_header = false;
            return _nothing;
        }

        if (st.at_line_start) {
            st.at_line_start = false;

            if (c == fasta_header_marker) {
                // a header yields a separator (also directly after a masked run)
                const bool yields = st.in_sequence || separator_before_first_record;
                st.in_header = true;
                st.in_sequence = true;
                st.in_masked_run = false;
                return yields ? _separator : _nothing;
            }

            if (lines_before_first_header) st.in_sequence = true;
        }

        if (st.in_header) return _nothing;
        if (!st.in_sequence) return _nothing; // body bytes before the first header
        return body_step(c, st, out);
    }

    /**
     * @brief processes a body byte of a record (see step), that is neither a line feed nor a dropped carriage return
     * @param c the input byte
     * @param st the state (updated)
     * @param out (output) the yielded symbol; only set if the result is _symbol
     * @return what the byte yields
     */
    inline step_t body_step(uint8_t c, state_t& st, uint8_t& out) const
    {
        const uint8_t u = lookup_byte(c);

        if (in_alphabet[u]) {
            st.in_masked_run = false;
            out = u;
            return _symbol;
        }

        // the first byte of a masked run yields the separator; the rest of the run yields nothing
        if (st.in_masked_run) return _nothing;
        st.in_masked_run = true;
        return _separator;
    }

    // returns the byte that a body byte is looked up as in the alphabet (upper-cased, if to_upper)
    inline uint8_t lookup_byte(uint8_t c) const
    {
        return (to_upper && c >= 'a' && c <= 'z') ? uint8_t(c - 'a' + 'A') : c;
    }

    // returns whether a body byte is masked (not in the alphabet, see body_step)
    inline bool masked(uint8_t c) const { return in_alphabet[lookup_byte(c)] == 0; }

    // returns whether every body byte yields itself (see body_step)
    bool keeps_body_bytes() const
    {
        return !to_upper && std::all_of(in_alphabet.begin(), in_alphabet.end(), [](uint8_t in) { return in != 0; });
    }

    /**
     * @brief returns the end of the run of bytes of the current line that starts at i: the first line feed in
     *        bytes[i..lim-1], or the first carriage return that yields nothing, or lim if there is none
     * @param bytes [0..n-1] bytes of the file
     * @param i the start of the run
     * @param lim the end of the bytes to search
     * @param n the number of bytes; bytes[lim] is the byte after the searched ones if lim < n
     */
    inline uint64_t line_run_end(const uint8_t* bytes, uint64_t i, uint64_t lim, uint64_t n) const
    {
        const void* line_feed = std::memchr(bytes + i, fasta_line_feed, lim - i);
        uint64_t j = line_feed == nullptr ? lim : uint64_t(static_cast<const uint8_t*>(line_feed) - bytes);

        if (carriage_return == _dropped) {
            const void* cr = std::memchr(bytes + i, fasta_carriage_return, j - i);
            if (cr != nullptr) j = uint64_t(static_cast<const uint8_t*>(cr) - bytes);
        } else if (carriage_return == _dropped_before_line_feed && j > i && bytes[j - 1] == fasta_carriage_return
            && j < n && bytes[j] == fasta_line_feed) {
            j--;
        }

        return j;
    }

    /**
     * @brief applies the transformation to bytes of a FASTA file; runs of header bytes and of body bytes within a
     *        line are passed to the handler at once, every other byte one at a time
     * @tparam handler_t type of the handler, with the methods
     *   - full(): returns whether the walk stops
     *   - body(bytes, len, i, st): processes body bytes (see body_step) bytes[0..len-1] = the bytes [i..i+len-1],
     *     updates st and returns how many of them it has processed (fewer than len stops the walk)
     *   - header(bytes, len): processes bytes of a header line after its '>', without its line end
     *   - step(i, c, before, after, step, symbol, after_cr): processes the byte c = bytes[i] with the states before
     *     and after it; step is what it yields, symbol the yielded symbol (the separator if step is _separator) and
     *     after_cr whether the byte before it is a carriage return
     * @param bytes [0..n-1] bytes of the file
     * @param i the first byte to process
     * @param lim the byte after the last one to process; bytes[lim] is the byte after it if lim < n
     * @param n the number of bytes
     * @param st the state before bytes[i] (updated)
     * @param h the handler
     * @return the byte after the last processed one
     */
    template <typename handler_t>
    inline uint64_t walk(const uint8_t* bytes, uint64_t i, uint64_t lim, uint64_t n, state_t& st, handler_t& h) const
    {
        while (i < lim && !h.full()) {
            if (!st.at_line_start) {
                const uint64_t j = line_run_end(bytes, i, lim, n);

                if (j > i) {
                    if (st.in_header) {
                        h.header(bytes + i, j - i);
                    } else if (st.in_sequence) {
                        const uint64_t processed = h.body(bytes + i, j - i, i, st);
                        if (processed < j - i) return i + processed;
                    }

                    i = j;
                    continue;
                }
            }

            const uint8_t c = bytes[i];
            const state_t before = st;
            uint8_t symbol = separator;
            const step_t result = step(c, st, symbol,
                [&] { return i + 1 < n && bytes[i + 1] == fasta_line_feed; });
            h.step(i, c, before, st, result, symbol, i > 0 && bytes[i - 1] == fasta_carriage_return);
            i++;
        }

        return i;
    }
};

/**
 * @brief a sampled text position of a FASTA file: the offset in the file of the byte that yields the symbol at that
 *        position and the state of the transformation before that byte; a reader of the text starts at the last
 *        sample at or before the position it reads (see fasta_text_reader)
 */
struct fasta_sample {
    uint64_t pos;    // position in the text
    uint64_t offset; // offset in the FASTA file of the byte yielding the symbol at pos
    uint8_t state;   // state of the transformation before that byte (see fasta_transform::state_t::to_bits)
};

/**
 * @brief a file in memory, read like a direct_reader
 */
class memory_file {
private:
    const uint8_t* data; // [0..n-1] the bytes of the file
    uint64_t n; // n, the size of the file

public:
    // number of bytes a window of the file has at most (see fasta_window): all of them
    static constexpr uint64_t block = std::numeric_limits<uint64_t>::max();

    memory_file(const uint8_t* data, uint64_t n) : data(data), n(n) { }

    // returns the size of the file in bytes
    uint64_t size() const { return n; }

    // returns the byte at a position
    uint8_t operator[](uint64_t pos) const { return data[pos]; }

    // returns a pointer to the bytes [beg, end)
    const uint8_t* read(uint64_t beg, uint64_t) const { return data + beg; }
};

/**
 * @brief a window of consecutive bytes of a FASTA file and the byte before them
 * @tparam bytes_t type of the file (direct_reader or memory_file)
 */
template <typename bytes_t>
struct fasta_window {
    const uint8_t* bytes = nullptr; // [0..n-1] the bytes [from, from + n) of the file
    uint64_t from = 0; // offset in the file of bytes[0]
    uint64_t begin = 0; // offset in the file of the first byte of the window after the byte before it
    uint64_t n = 0; // number of bytes of the window
    // offset after the last byte whose successor is in the window or that is the last byte of the file
    uint64_t limit = 0;

    /**
     * @brief loads the window that starts at a position, with the byte before it
     * @param in the file
     * @param pos [0..size-1] the position
     */
    void load(bytes_t& in, uint64_t pos)
    {
        const uint64_t size = in.size();
        begin = pos;
        from = pos - (pos > 0 ? 1 : 0);
        const uint64_t end = pos + std::min(bytes_t::block, size - pos);
        n = end - from;
        bytes = in.read(from, end);
        limit = end == size ? end : end - 1;
    }

    // returns whether the bytes at pos and after it can be processed without loading another window
    bool contains(uint64_t pos) const { return bytes != nullptr && pos >= begin && pos < limit; }

    /**
     * @brief applies a transformation to the bytes [pos, min(lim, limit)) of the window (see fasta_transform::walk)
     * @return the offset in the file after the last processed byte
     */
    template <typename handler_t>
    uint64_t walk(const fasta_transform& transform, uint64_t pos, uint64_t lim, fasta_transform::state_t& st,
        handler_t& h) const
    {
        return from + transform.walk(bytes, pos - from, std::min(lim, limit) - from, n, st, h);
    }
};

/**
 * @brief the result of scanning a chunk (a byte range) of a FASTA file (see scan_fasta_range). If a masked run
 *        contains the boundary between two chunks, both chunks yield a separator for it; the separator of the second
 *        chunk is dropped when the chunks are merged (see append_fasta_chunk).
 */
struct fasta_chunk {
    uint64_t text_size = 0; // length of the text of the chunk
    // [0..255] freq[c] = number of occurrences of the byte c in the text of the chunk
    std::array<uint64_t, 256> freq {};
    // the samples, with positions relative to the start of the text of the chunk, in increasing order of their
    // positions
    std::vector<fasta_sample> samples;
    bool starts_in_masked_run = false; // the first symbol of the chunk is the separator of a masked run
    bool ends_in_masked_run = false; // the chunk ends in a masked run
    fasta_transform::state_t start_state; // the state before the first byte of the chunk (see scan_fasta_lines)
    fasta_transform::state_t end_state; // the state after the last byte of the chunk (see scan_fasta_lines)

    // the line events of scan_fasta_lines; a derived chunk replaces them
    void header_start() { } // the '>' of a header line
    void header_bytes(const uint8_t*, uint64_t) { } // bytes of a header line (see fasta_transform::walk)
    void sequence_symbols(uint64_t) { } // a number of body bytes of the current line that yield a symbol
    void line_end(bool, bool) { } // a line feed ends a header line (true) or another line; after a carriage return
};

/** @brief the length, the symbol frequencies and the samples of the text of a FASTA file */
struct fasta_scan {
    uint64_t text_size = 0; // length of the text
    std::array<uint64_t, 256> freq {}; // [0..255] freq[c] = number of occurrences of the byte c in the text
    std::vector<fasta_sample> samples; // the samples, in increasing order of their positions
    bool ends_in_masked_run = false; // the text ends in a masked run
};

/**
 * @brief appends the text of a chunk to the text before it; the first symbol of the chunk is dropped if it is the
 *        separator of a masked run that the text before it ends in
 * @param text the text before the chunk (updated)
 * @param chunk the chunk directly after it (its text_size and freq are decreased if its first symbol is dropped)
 * @param separator the separator
 * @return the position in the text of the position 0 of the chunk (if its first symbol is dropped, the position
 *         before its second symbol)
 */
inline uint64_t append_fasta_chunk(fasta_scan& text, fasta_chunk& chunk, uint8_t separator)
{
    const uint64_t drop = chunk.starts_in_masked_run && text.ends_in_masked_run ? 1 : 0;
    if (chunk.text_size != 0) text.ends_in_masked_run = chunk.ends_in_masked_run;
    const uint64_t shift = text.text_size - drop;
    chunk.text_size -= drop;
    chunk.freq[separator] -= drop;
    for (uint64_t c = 0; c < text.freq.size(); c++) text.freq[c] += chunk.freq[c];

    for (fasta_sample sample : chunk.samples) {
        sample.pos += shift;
        text.samples.push_back(sample);
    }

    text.text_size += chunk.text_size;
    return shift;
}

// parameters of scan_fasta_lines
struct fasta_scan_params {
    uint64_t num_threads = 1; // maximum number of threads
    uint64_t sample_period = fasta_sample_period; // a sample every sample_period positions (0: none)
    uint64_t min_chunk_bytes = fasta_min_chunk_bytes; // minimum number of bytes of a chunk
    phase_progress* progress = nullptr; // if not nullptr, counts the bytes scanned
};

/**
 * @brief the handler of fasta_transform::walk that adds the symbols, samples and line events of a byte range of a FASTA
 *        file to a chunk (see scan_fasta_range); with writes, it writes the symbols to dst instead of counting their
 *        frequencies and stops the walk when dst is full
 * @tparam chunk_t type of the chunk (derived from fasta_chunk)
 * @tparam writes whether the symbols are written to dst
 */
template <typename chunk_t, bool writes = false>
class fasta_range_scanner {
private:
    using state_t = fasta_transform::state_t;
    using step_t = fasta_transform::step_t;

    // (writes) the value of body_symbol for a masked byte
    static constexpr uint16_t masked_body = 256;

    chunk_t& out; // the chunk
    const fasta_transform& transform; // the transformation
    const bool keeps; // whether every body byte yields itself (see fasta_transform::keeps_body_bytes)
    const uint64_t sample_period; // a sample every sample_period positions of the chunk (0: none)
    uint64_t next_sample; // position of the next sample
    bool first_symbol = true; // no symbol has been yielded yet
    // (writes) [0..255] body_symbol[c] = the symbol that the body byte c yields, or masked_body if it is masked
    std::array<uint16_t, 256> body_symbol {};

public:
    uint64_t from = 0; // offset in the file of the first byte passed to walk
    uint8_t* dst = nullptr; // (writes) the destination of the symbols
    uint64_t capacity = 0; // (writes) the number of symbols that fit into dst
    uint64_t written = 0; // (writes) the number of symbols written to dst

private:
    // adds a symbol that the byte at offset yields, where before is the state before it
    void yield(uint8_t symbol, uint64_t offset, const state_t& before, step_t step, bool in_header_after)
    {
        // the first symbol of the chunk is the separator of a masked run (the run may start before the chunk)
        if (step == fasta_transform::_separator && first_symbol && !in_header_after && !before.in_masked_run) {
            out.starts_in_masked_run = true;
        }

        first_symbol = false;
        if constexpr (!writes) out.freq[symbol]++;

        if (out.text_size == next_sample && sample_period != 0) {
            out.samples.push_back(fasta_sample { out.text_size, offset, before.to_bits() });
            next_sample += sample_period;
        }

        if constexpr (writes) dst[written++] = symbol;
        out.text_size++;
    }

    /* (writes) processes the body bytes of body() if they are not kept as they are (see fasta_transform::body_step):
       writes their symbols to dst; at most min(len, capacity - written) bytes are processed (each yields at most one
       symbol) */
    uint64_t write_body(const uint8_t* bytes, uint64_t len, uint64_t i, state_t& st)
    {
        uint8_t* const sym = dst + written;
        const uint64_t end = std::min(len, capacity - written);
        const uint8_t separator = transform.separator;
        const uint64_t base = out.text_size; // the position in the chunk of sym[0]
        // the position of the next sample relative to base (none if sample_period is 0)
        uint64_t sample_at = sample_period != 0 ? next_sample - base : std::numeric_limits<uint64_t>::max();
        bool masked = st.in_masked_run;
        uint64_t k = 0; // number of symbols written
        uint64_t x = 0; // number of bytes processed
        uint64_t letters = 0; // number of bytes that yield themselves

        for (; x < end; x++) {
            const uint16_t s = body_symbol[bytes[x]];
            const bool is_masked = s == masked_body;
            if (is_masked && masked) continue;

            if (k == sample_at) [[unlikely]] {
                state_t before = st;
                before.in_masked_run = masked;
                out.samples.push_back(fasta_sample { base + k, from + i + x, before.to_bits() });
                sample_at += sample_period;
            }

            // the first symbol of the chunk is the separator of a masked run
            if (is_masked && first_symbol && k == 0) [[unlikely]] out.starts_in_masked_run = true;
            sym[k++] = is_masked ? separator : uint8_t(s);
            letters += !is_masked;
            masked = is_masked;
        }

        if (k != 0) first_symbol = false;
        if (sample_period != 0) next_sample = base + sample_at;
        st.in_masked_run = masked;
        out.text_size += k;
        written += k;
        out.sequence_symbols(letters);
        return x;
    }

public:
    fasta_range_scanner(chunk_t& out, const fasta_transform& transform, uint64_t sample_period)
        : out(out), transform(transform), keeps(transform.keeps_body_bytes()), sample_period(sample_period),
          next_sample(sample_period)
    {
        if constexpr (writes) {
            for (uint64_t c = 0; c < body_symbol.size(); c++) {
                const uint8_t u = transform.lookup_byte(uint8_t(c));
                body_symbol[c] = transform.in_alphabet[u] ? u : masked_body;
            }
        }
    }

    bool full() const
    {
        if constexpr (writes) return written == capacity;
        return false;
    }

    uint64_t body(const uint8_t* bytes, uint64_t len, uint64_t i, state_t& st)
    {
        if (keeps) {
            if constexpr (writes) {
                len = std::min(len, capacity - written);
                std::memcpy(dst + written, bytes, len);
                written += len;
            } else {
                for (uint64_t x = 0; x < len; x++) out.freq[bytes[x]]++;
            }

            for (; sample_period != 0 && next_sample < out.text_size + len; next_sample += sample_period) {
                const uint64_t x = next_sample - out.text_size;
                state_t before = st;
                if (x > 0) before.in_masked_run = false;
                out.samples.push_back(fasta_sample { next_sample, from + i + x, before.to_bits() });
            }

            first_symbol = false;
            out.text_size += len;
            st.in_masked_run = false;
            out.sequence_symbols(len);
            return len;
        }

        if constexpr (writes) return write_body(bytes, len, i, st);
        uint64_t symbols = 0; // number of bytes that yield a symbol
        uint64_t x = 0; // number of bytes processed

        for (; x < len; x++) {
            const state_t before = st;
            uint8_t symbol = transform.separator;
            const step_t step = transform.body_step(bytes[x], st, symbol);
            if (step != fasta_transform::_nothing) yield(symbol, from + i + x, before, step, false);
            if (step == fasta_transform::_symbol) symbols++;
        }

        out.sequence_symbols(symbols);
        return x;
    }

    void header(const uint8_t* bytes, uint64_t len) { out.header_bytes(bytes, len); }

    void step(uint64_t i, uint8_t c, const state_t& before, const state_t& after, step_t step, uint8_t symbol,
        bool after_cr)
    {
        if (c == fasta_line_feed) out.line_end(before.in_header, after_cr);
        else if (after.in_header && !before.in_header) out.header_start();
        if (step == fasta_transform::_nothing) return;
        yield(symbol, from + i, before, step, after.in_header);
        if (step == fasta_transform::_symbol) out.sequence_symbols(1);
    }
};

/**
 * @brief applies the transformation to a byte range of a FASTA file, which can start and end inside a line; the
 *        lines are passed to the chunk (see the line events of fasta_chunk)
 * @tparam bytes_t type of the file (direct_reader or memory_file)
 * @tparam chunk_t type of the result (derived from fasta_chunk)
 * @param in the file
 * @param begin first byte of the range
 * @param end byte after the last byte of the range
 * @param transform the transformation
 * @param out the result; out.start_state is the state before begin; out.end_state is set to the state after end - 1
 * @param sample_period a sample is taken every sample_period positions of the chunk (0: none); the position 0 of
 *        the chunk is never sampled, since the state before its first symbol depends on the chunk before
 * @param progress if not nullptr, counts the bytes scanned
 */
template <typename bytes_t, typename chunk_t>
inline void scan_fasta_range(bytes_t& in, uint64_t begin, uint64_t end, const fasta_transform& transform,
    chunk_t& out, uint64_t sample_period, phase_progress* progress)
{
    fasta_range_scanner<chunk_t> scanner(out, transform, sample_period);
    fasta_transform::state_t st = out.start_state;
    fasta_window<bytes_t> window;

    for (uint64_t pos = begin; pos < end;) {
        window.load(in, pos);
        scanner.from = window.from;
        const uint64_t next = window.walk(transform, pos, end, st, scanner);
        if (progress != nullptr) progress->advance(next - pos);
        pos = next;
    }

    out.end_state = st;
    out.ends_in_masked_run = st.in_masked_run;
}

/**
 * @brief scans a FASTA file in parallel and passes its lines to the chunks (see the line events of fasta_chunk): the
 *        file is split into chunks of at least min_chunk_bytes bytes at arbitrary offsets, and rounds of up to
 *        num_threads consecutive chunks are scanned at the same time, one chunk per thread (see scan_fasta_range).
 *        A chunk after the first one is scanned as if a record started before it, at a line start if the byte before
 *        it is a line feed and inside a sequence line else; after each round, a chunk whose state at its start
 *        differs from the state at the end of the chunk before it is scanned again, then the chunks are appended to
 *        the text in the order of their positions (see append_fasta_chunk).
 * @tparam chunk_t type of the result of a chunk (derived from fasta_chunk)
 * @param open returns a new reader of the file (a direct_reader or a memory_file)
 * @param transform the transformation
 * @param prm parameters; if sample_period != 0, the first sample is at position 0
 * @param merge called as merge(chunk, shift) for each chunk in the order of their positions, after the chunk has
 *        been appended; shift is the position in the text of the position 0 of the chunk
 * @return the text
 */
template <typename chunk_t, typename open_fnc_t, typename merge_fnc_t>
inline fasta_scan scan_fasta_lines(open_fnc_t open, const fasta_transform& transform, const fasta_scan_params& prm,
    merge_fnc_t merge)
{
    using bytes_t = decltype(open());
    using state_t = fasta_transform::state_t;
    fasta_scan text;
    if (prm.sample_period != 0) text.samples.push_back(fasta_sample { 0, 0, state_t {}.to_bits() });
    std::vector<bytes_t> readers; // [0..p-1] the reader of each thread
    readers.emplace_back(open());
    const uint64_t size = readers[0].size();
    if (size == 0) return text;
    const uint64_t num_chunks = std::max<uint64_t>(1, size / std::max<uint64_t>(1, prm.min_chunk_bytes));
    const uint64_t p = std::max<uint64_t>(1, std::min(prm.num_threads, num_chunks));
    for (uint64_t t = 1; t < p; t++) readers.emplace_back(open());
    std::vector<chunk_t> chunks(p); // [0..q-1] the chunks of the current round
    state_t state; // the state after the chunks appended so far

    // returns the first byte of chunk k
    auto chunk_start = [&](uint64_t k) { return k == num_chunks ? size : k * (size / num_chunks); };

    // returns whether two states agree in everything but in_masked_run (see append_fasta_chunk)
    auto same_line_state = [](const state_t& a, const state_t& b) {
        return a.at_line_start == b.at_line_start && a.in_header == b.in_header && a.in_sequence == b.in_sequence;
    };

    for (uint64_t first = 0; first < num_chunks; first += p) {
        const uint64_t q = std::min(p, num_chunks - first); // the number of chunks of the round

        #pragma omp parallel for num_threads(q) schedule(static, 1)
        for (uint64_t k = first; k < first + q; k++) {
            bytes_t& in = readers[omp_get_thread_num()];
            chunk_t& chunk = chunks[k - first];
            chunk = chunk_t();
            const uint64_t begin = chunk_start(k);

            if (k > 0) {
                chunk.start_state.at_line_start = in[begin - 1] == fasta_line_feed;
                chunk.start_state.in_sequence = true;
            }

            scan_fasta_range(in, begin, chunk_start(k + 1), transform, chunk, prm.sample_period,
                prm.progress);
        }

        for (uint64_t k = 0; k < q; k++) {
            chunk_t& chunk = chunks[k];

            if (!same_line_state(chunk.start_state, state)) {
                chunk = chunk_t();
                chunk.start_state = state;
                chunk.start_state.in_masked_run = false;
                scan_fasta_range(readers[0], chunk_start(first + k), chunk_start(first + k + 1), transform,
                    chunk, prm.sample_period, nullptr);
            }

            state = chunk.end_state;
            merge(chunk, append_fasta_chunk(text, chunk, transform.separator));
        }
    }

    return text;
}

/**
 * @brief the handler of fasta_transform::walk that writes the yielded symbols to dst (or skips them, if dst is nullptr)
 *        and stops the walk when len symbols have been written
 */
struct fasta_symbol_writer {
    using state_t = fasta_transform::state_t;
    using step_t = fasta_transform::step_t;

    const fasta_transform& transform; // the transformation
    const bool keeps; // whether every body byte yields itself (see fasta_transform::keeps_body_bytes)
    uint8_t* dst; // the destination
    uint64_t len; // the number of symbols to write
    uint64_t written = 0; // the number of symbols written

    bool full() const { return written == len; }

    uint64_t body(const uint8_t* bytes, uint64_t n, uint64_t, state_t& st)
    {
        if (keeps) {
            const uint64_t copied = std::min(n, len - written);
            if (dst != nullptr) std::memcpy(dst + written, bytes, copied);
            written += copied;
            if (copied > 0) st.in_masked_run = false;
            return copied;
        }

        uint64_t x = 0;

        for (; x < n && written < len; x++) {
            uint8_t symbol = transform.separator;
            if (transform.body_step(bytes[x], st, symbol) == fasta_transform::_nothing) continue;
            if (dst != nullptr) dst[written] = symbol;
            written++;
        }

        return x;
    }

    void header(const uint8_t*, uint64_t) { }

    void step(uint64_t, uint8_t, const state_t&, const state_t&, step_t step, uint8_t symbol, bool)
    {
        if (step == fasta_transform::_nothing) return;
        if (dst != nullptr) dst[written] = symbol;
        written++;
    }
};

/**
 * @brief reads the text of a FASTA file (see fasta_transform); a read starts at the last sample at or before the
 *        position it reads (see fasta_sample), and consecutive reads of consecutive ranges continue the scan of the
 *        file
 * @tparam bytes_t type of the file (direct_reader or memory_file)
 */
template <typename bytes_t>
class fasta_text_reader {
private:
    using state_t = fasta_transform::state_t;
    using step_t = fasta_transform::step_t;

    const fasta_transform& transform; // the transformation
    bool keeps; // whether every body byte yields itself (see fasta_transform::keeps_body_bytes)
    const fasta_sample* samples; // [0..num_samples-1] the samples, in increasing order of their positions
    uint64_t num_samples; // the number of samples
    bytes_t file; // the file
    fasta_window<bytes_t> window; // the bytes of the file around offset
    uint64_t offset = 0; // offset in the file of the next byte to scan
    uint64_t next = std::numeric_limits<uint64_t>::max(); // position in the text of the next symbol the scan yields
    state_t state; // state of the transformation before the byte at offset

    // continues the scan at the last sample at or before pos and stops before the symbol at pos
    void seek(uint64_t pos)
    {
        const fasta_sample* s = std::upper_bound(samples, samples + num_samples, pos,
            [](uint64_t p, const fasta_sample& sample) { return p < sample.pos; }) - 1;
        offset = s->offset;
        next = s->pos;
        state = state_t::from_bits(s->state);
        if (next < pos) scan(pos - next, nullptr);
    }

    // writes the next len symbols of the text to dst (skips them, if dst is nullptr)
    void scan(uint64_t len, uint8_t* dst)
    {
        fasta_symbol_writer writer { transform, keeps, dst, len };

        while (!writer.full()) {
            if (offset >= file.size()) throw std::runtime_error("the FASTA file ends before its text does");
            if (!window.contains(offset)) window.load(file, offset);
            offset = window.walk(transform, offset, window.limit, state, writer);
        }

        next += len;
    }

public:
    /**
     * @brief starts reading the text of a FASTA file
     * @param file the file
     * @param transform the transformation (it must stay valid while the reader is used)
     * @param samples [0..num_samples-1] the samples of the text in increasing order of their positions, the first one
     *        at position 0 (they must stay valid while the reader is used)
     * @param num_samples the number of samples (at least 1)
     */
    fasta_text_reader(bytes_t&& file, const fasta_transform& transform, const fasta_sample* samples,
        uint64_t num_samples)
        : transform(transform), keeps(transform.keeps_body_bytes()), samples(samples), num_samples(num_samples),
          file(std::move(file)) { }

    /**
     * @brief writes the symbols [pos, pos + len) of the text to dst
     * @param pos the first position
     * @param len the number of symbols
     * @param dst the destination of at least len symbols
     */
    void read(uint64_t pos, uint64_t len, uint8_t* dst)
    {
        if (pos != next) seek(pos);
        scan(len, dst);
    }
};

/**
 * @brief follows the state of the transformation over bytes of a FASTA file (see fasta_transform::walk) without
 *        yielding symbols; the state may be unknown in in_masked_run at the start, which the next body byte or header
 *        line settles (known)
 */
struct fasta_state_walker {
    using state_t = fasta_transform::state_t;
    using step_t = fasta_transform::step_t;

    const fasta_transform& transform; // the transformation
    bool known; // whether in_masked_run is known
    bool stop_when_known = false; // the walk stops directly after the byte that settles in_masked_run
    bool stop_at_header = false; // the walk stops directly after the '>' of the first header line
    bool stop_after_line_start = false; // the walk stops directly after the byte that ends the start of a line
    bool header_found = false; // a header line has started
    bool line_started = false; // a byte has ended the start of a line
    uint64_t header_offset = 0; // offset in the file of the '>' of that header line
    uint64_t from = 0; // offset in the file of the first byte passed to walk

    bool full() const
    {
        return (stop_when_known && known) || (stop_at_header && header_found)
            || (stop_after_line_start && line_started);
    }

    uint64_t body(const uint8_t* bytes, uint64_t len, uint64_t, state_t& st)
    {
        if (stop_when_known && !known) len = 1;
        st.in_masked_run = transform.masked(bytes[len - 1]);
        known = true;
        return len;
    }

    void header(const uint8_t*, uint64_t) { }

    void step(uint64_t i, uint8_t, const state_t& before, const state_t& after, step_t step, uint8_t, bool)
    {
        if (before.at_line_start && !after.at_line_start) line_started = true;

        if (after.in_header) {
            if (before.in_header || header_found) return;
            header_found = true;
            header_offset = from + i;
            known = true;
        } else if (after.in_sequence && (step != fasta_transform::_nothing || after.in_masked_run)) {
            known = true; // a body byte (see fasta_transform::body_step)
        }
    }
};

/**
 * @brief returns the offset in a FASTA file of the '>' of its first header line (the size of the file if it has none)
 * @param in the file
 * @param transform the transformation
 */
template <typename bytes_t>
inline uint64_t fasta_first_header(bytes_t& in, const fasta_transform& transform)
{
    fasta_state_walker walker { transform, true };
    walker.stop_at_header = true;
    fasta_transform::state_t st;
    fasta_window<bytes_t> window;
    const uint64_t size = in.size();

    for (uint64_t pos = 0; pos < size && !walker.header_found;) {
        window.load(in, pos);
        walker.from = window.from;
        pos = window.walk(transform, pos, window.limit, st, walker);
    }

    return walker.header_found ? walker.header_offset : size;
}

/** @brief the first byte of a range of a FASTA file and the state of the transformation before it */
struct fasta_range_bound {
    uint64_t offset = 0; // offset in the file of the byte
    fasta_transform::state_t state; // the state before it
};

/**
 * @brief returns the first offset at or after pos at which the state of the transformation follows from the line it
 *        is in, so that a range of the file starting there can be read without the bytes before that line: the state
 *        at the start of the line is known except in_masked_run, the line is walked to pos (if pos lies more than a
 *        window after the start of the line, only the first byte of the line, which tells its type, and the byte
 *        before pos) and on until the next body byte or header line settles in_masked_run
 * @param in the file
 * @param pos [1..size-1] the offset
 * @param line the start of the line containing pos (the offset after the last line feed before pos, or 0)
 * @param transform the transformation
 * @param first_header the offset of the first header line (see fasta_first_header)
 * @return the offset (the size of the file, if there is none) and the state before it
 */
template <typename bytes_t>
inline fasta_range_bound fasta_range_start(bytes_t& in, uint64_t pos, uint64_t line, const fasta_transform& transform,
    uint64_t first_header)
{
    using state_t = fasta_transform::state_t;
    const uint64_t size = in.size();
    state_t st; // the state at the start of the line, apart from in_masked_run
    st.in_sequence = line > first_header || (transform.lines_before_first_header && line > 0);
    fasta_state_walker walker { transform, !st.in_sequence };
    fasta_window<bytes_t> window;
    uint64_t offset = line;

    if (pos - line > bytes_t::block) {
        walker.stop_after_line_start = true;

        while (offset < pos && !walker.line_started) {
            if (!window.contains(offset)) window.load(in, offset);
            walker.from = window.from;
            offset = window.walk(transform, offset, pos, st, walker);
        }

        // in a header line in_masked_run is false; in a sequence line the byte before pos settles it
        walker.stop_after_line_start = false;
        offset = std::max(offset, pos - 1);
        st.in_masked_run = false;
        walker.known = st.in_header || !st.in_sequence;
    }

    while (offset < size && (offset < pos || !walker.known)) {
        if (!window.contains(offset)) window.load(in, offset);
        walker.from = window.from;
        walker.stop_when_known = offset >= pos;
        offset = window.walk(transform, offset, offset < pos ? pos : window.limit, st, walker);
    }

    return { offset, st };
}

/**
 * @brief returns the starts of ranges of a FASTA file (see fasta_range_start): the start of the line containing
 *        positions[i] is searched backward down to positions[i - 1] only, then the starts of the lines are taken over
 *        from the positions before, so each byte of the file is searched at most once
 * @param open returns a new reader of the file (a direct_reader or a memory_file)
 * @param transform the transformation
 * @param positions [0..q-1] the offsets in increasing order (positions[0] = 0, the start of the first range)
 * @param num_threads the number of threads
 * @return [0..q-1] the starts of the ranges
 */
template <typename open_fnc_t>
inline std::vector<fasta_range_bound> fasta_range_starts(open_fnc_t open, const fasta_transform& transform,
    const std::vector<uint64_t>& positions, uint64_t num_threads)
{
    using bytes_t = decltype(open());
    constexpr uint64_t no_line_feed = std::numeric_limits<uint64_t>::max();
    const uint64_t q = positions.size();
    std::vector<fasta_range_bound> bounds(q);
    bytes_t file = open();
    const uint64_t size = file.size();
    const uint64_t first_header = fasta_first_header(file, transform);
    // [1..q-1] the offset after the last line feed in [positions[i - 1], positions[i]), or no_line_feed
    std::vector<uint64_t> after_line_feed(q, no_line_feed);

    #pragma omp parallel for num_threads(num_threads) schedule(dynamic, 1)
    for (uint64_t i = 1; i < q; i++) {
        bytes_t in = open();

        for (uint64_t end = std::min(positions[i], size); end > positions[i - 1];) {
            const uint64_t beg = end - std::min(end - positions[i - 1], bytes_t::block);
            const uint8_t* bytes = in.read(beg, end);
            const std::reverse_iterator<const uint8_t*> last(bytes + (end - beg)), first(bytes);
            const auto line_feed = std::find(last, first, fasta_line_feed);

            if (line_feed != first) {
                after_line_feed[i] = beg + uint64_t(line_feed.base() - bytes);
                break;
            }

            end = beg;
        }
    }

    // [0..q-1] the start of the line containing positions[i]
    std::vector<uint64_t> line(q, 0);
    for (uint64_t i = 1; i < q; i++) line[i] = after_line_feed[i] != no_line_feed ? after_line_feed[i] : line[i - 1];

    #pragma omp parallel for num_threads(num_threads) schedule(dynamic, 1)
    for (uint64_t i = 1; i < q; i++) {
        if (positions[i] >= size) {
            bounds[i].offset = size;
            continue;
        }

        bytes_t in = open();
        bounds[i] = fasta_range_start(in, positions[i], line[i], transform, first_header);
    }

    return bounds;
}

/**
 * @brief reads the text of a FASTA file from the start of a range on (see fasta_range_start) in blocks: first the text
 *        of the range, whose length, samples and line events go to a chunk (see scan_fasta_range; not the symbol
 *        frequencies, see fasta_range_scanner), then the text after the range; no block contains symbols of both
 * @tparam bytes_t type of the file (direct_reader or memory_file)
 * @tparam chunk_t type of the chunk (derived from fasta_chunk)
 */
template <typename bytes_t, typename chunk_t = fasta_chunk>
class fasta_range_reader {
private:
    const fasta_transform& transform; // the transformation
    const bool keeps; // whether every body byte yields itself (see fasta_transform::keeps_body_bytes)
    bytes_t file; // the file
    fasta_window<bytes_t> window; // the bytes of the file around offset
    uint64_t offset; // offset in the file of the next byte to read
    uint64_t end; // offset in the file after the last byte of the range
    fasta_transform::state_t state; // the state of the transformation before the byte at offset
    chunk_t& chunk; // the chunk of the range
    fasta_range_scanner<chunk_t, true> scanner; // reads the text of the range
    phase_progress* progress; // if not nullptr, counts the bytes of the range read

public:
    /**
     * @brief starts reading the text of a range
     * @param file the file
     * @param transform the transformation (it must stay valid while the reader is used)
     * @param begin the start of the range (see fasta_range_start)
     * @param end the offset after the last byte of the range
     * @param chunk the chunk of the range (reset here)
     * @param sample_period a sample every sample_period positions of the range (0: none, see scan_fasta_range)
     * @param progress if not nullptr, counts the bytes of the range read
     */
    fasta_range_reader(bytes_t&& file, const fasta_transform& transform, const fasta_range_bound& begin,
        uint64_t end, chunk_t& chunk, uint64_t sample_period, phase_progress* progress)
        : transform(transform), keeps(transform.keeps_body_bytes()), file(std::move(file)), offset(begin.offset),
          end(end), state(begin.state), chunk(chunk), scanner(chunk, transform, sample_period), progress(progress)
    {
        chunk = chunk_t();
        chunk.start_state = begin.state;
    }

    // returns whether the bytes of the range have been read (then the chunk is complete)
    bool range_read() const { return offset >= end; }

    /**
     * @brief writes the next at most len symbols of the text to dst; the symbols of the range and the ones after it
     *        are not written by the same call
     * @param dst the destination
     * @param len [1..] the number of symbols
     * @return the number of symbols written (0: the text has ended)
     */
    uint64_t read(uint8_t* dst, uint64_t len)
    {
        if (offset < end) {
            scanner.dst = dst;
            scanner.capacity = len;
            scanner.written = 0;

            while (offset < end && !scanner.full()) {
                if (!window.contains(offset)) window.load(file, offset);
                scanner.from = window.from;
                const uint64_t next = window.walk(transform, offset, end, state, scanner);
                if (progress != nullptr) progress->advance(next - offset);
                offset = next;
            }

            if (offset >= end) {
                chunk.end_state = state;
                chunk.ends_in_masked_run = state.in_masked_run;
            }

            if (scanner.written != 0) return scanner.written;
        }

        fasta_symbol_writer writer { transform, keeps, dst, len };

        while (offset < file.size() && !writer.full()) {
            if (!window.contains(offset)) window.load(file, offset);
            offset = window.walk(transform, offset, window.limit, state, writer);
        }

        return writer.written;
    }
};

#endif // STRING_UTILS_FASTA_SCAN_HPP
