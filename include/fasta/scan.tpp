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

#ifndef STRING_UTILS_FASTA_SCAN_TPP
#define STRING_UTILS_FASTA_SCAN_TPP

#include <fasta/scan.hpp>

inline auto fasta_transform::state_t::from_bits(uint8_t bits) -> state_t
{
    return state_t {
        .at_line_start = (bits & 1) != 0,
        .in_header = (bits & 2) != 0,
        .in_sequence = (bits & 4) != 0,
        .in_masked_run = (bits & 8) != 0 };
}

template <typename line_feed_follows_t>
inline auto fasta_transform::step(uint8_t c, state_t& st, uint8_t& out, line_feed_follows_t line_feed_follows) const -> step_t
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

inline auto fasta_transform::body_step(uint8_t c, state_t& st, uint8_t& out) const -> step_t
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

inline uint64_t fasta_transform::line_run_end(const uint8_t* bytes, uint64_t i, uint64_t lim, uint64_t n) const
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

template <typename handler_t>
inline uint64_t fasta_transform::walk(const uint8_t* bytes, uint64_t i, uint64_t lim, uint64_t n, state_t& st, handler_t& h) const
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

template <typename bytes_t>
inline void fasta_window<bytes_t>::load(bytes_t& in, uint64_t pos)
{
    const uint64_t size = in.size();
    begin = pos;
    from = pos - (pos > 0 ? 1 : 0);
    const uint64_t end = pos + std::min(bytes_t::block, size - pos);
    n = end - from;
    bytes = in.read(from, end);
    limit = end == size ? end : end - 1;
}

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

template <typename chunk_t, bool writes>
inline void fasta_range_scanner<chunk_t, writes>::yield(uint8_t symbol, uint64_t offset,
    const state_t& before, step_t step, bool in_header_after)
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

template <typename chunk_t, bool writes>
inline uint64_t fasta_range_scanner<chunk_t, writes>::write_body(const uint8_t* bytes, uint64_t len, uint64_t i, state_t& st)
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

template <typename chunk_t, bool writes>
inline fasta_range_scanner<chunk_t, writes>::fasta_range_scanner(chunk_t& out, const fasta_transform& transform, uint64_t sample_period)
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

template <typename chunk_t, bool writes>
inline uint64_t fasta_range_scanner<chunk_t, writes>::body(const uint8_t* bytes, uint64_t len, uint64_t i, state_t& st)
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

template <typename chunk_t, bool writes>
inline void fasta_range_scanner<chunk_t, writes>::step(uint64_t i, uint8_t c, const state_t& before, const state_t& after, step_t step, uint8_t symbol,
    bool after_cr)
{
    if (c == fasta_line_feed) out.line_end(before.in_header, after_cr);
    else if (after.in_header && !before.in_header) out.header_start();
    if (step == fasta_transform::_nothing) return;
    yield(symbol, from + i, before, step, after.in_header);
    if (step == fasta_transform::_symbol) out.sequence_symbols(1);
}

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

template <typename chunk_t, typename open_fnc_t, typename merge_fnc_t>
inline fasta_scan scan_fasta_lines(open_fnc_t open, const fasta_transform& transform, const fasta_scan_params& prm, merge_fnc_t merge)
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

inline uint64_t fasta_symbol_writer::body(const uint8_t* bytes, uint64_t n, uint64_t, state_t& st)
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

template <typename bytes_t>
inline void fasta_text_reader<bytes_t>::seek(uint64_t pos)
{
    const fasta_sample* s = std::upper_bound(samples, samples + num_samples, pos,
        [](uint64_t p, const fasta_sample& sample) { return p < sample.pos; }) - 1;
    offset = s->offset;
    next = s->pos;
    state = state_t::from_bits(s->state);
    if (next < pos) scan(pos - next, nullptr);
}

template <typename bytes_t>
inline void fasta_text_reader<bytes_t>::scan(uint64_t len, uint8_t* dst)
{
    fasta_symbol_writer writer { transform, keeps, dst, len };

    while (!writer.full()) {
        if (offset >= file.size()) throw std::runtime_error("the FASTA file ends before its text does");
        if (!window.contains(offset)) window.load(file, offset);
        offset = window.walk(transform, offset, window.limit, state, writer);
    }

    next += len;
}

inline void fasta_state_walker::step(uint64_t i, uint8_t, const state_t& before, const state_t& after, step_t step, uint8_t, bool)
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

template <typename bytes_t, typename chunk_t>
inline uint64_t fasta_range_reader<bytes_t, chunk_t>::read(uint8_t* dst, uint64_t len)
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

#endif // STRING_UTILS_FASTA_SCAN_TPP
