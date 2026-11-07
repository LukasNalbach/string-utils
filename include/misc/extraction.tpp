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
 * @file extraction.tpp
 * @brief extracting ranges of a compressed text to files
 */

#ifndef STRING_UTILS_MISC_EXTRACTION_TPP
#define STRING_UTILS_MISC_EXTRACTION_TPP

#include <misc/extraction.hpp>

inline bool direct_extraction_file::open_file(const std::string& path, uint64_t length)
{
    close_file();
    const uint64_t largest_aligned_offset = platform::max_file_offset / sector * sector;
    if (length == 0 || length > largest_aligned_offset) return false;

    bool direct = false;
    fd = open_direct_or_buffered(path, platform::file_access::read_write, direct);
    if (fd < 0) return false;
    if (!direct) advise_uncached(fd);
    // the writers write far apart (see platform::make_sparse)
    platform::make_sparse(fd);

    logical_length = length;
    physical_length = div_ceil<uint64_t>(length, sector) * sector;
    if (!platform::set_size(fd, physical_length)) {
        platform::close(fd); fd = -1; logical_length = physical_length = 0; return false;
    }
    return true;
}

inline direct_extraction_file::writer::writer(direct_extraction_file& file, uint64_t begin, uint64_t end,
    std::mutex* begin_mutex, std::mutex* end_mutex)
    : file(file), begin(begin), pos(begin), end(end),
    begin_mutex(begin_mutex), end_mutex(end_mutex)
{
    if (begin > end || end > file.logical_length)
        throw std::invalid_argument("an extraction writer is outside the output file");
    write_buffer = static_cast<char*>(allocate_aligned(write_block_length));

    try {
        read_buffer = static_cast<char*>(allocate_aligned(read_buffer_length));
    } catch (...) {
        platform::free_aligned(write_buffer);
        throw;
    }
}

inline void direct_extraction_file::writer::write(const char* data, uint64_t length)
{
    if (length > end - pos) throw std::runtime_error("extraction wrote past its file interval");

    while (length > 0) {
        if (cached_block == none) select_block();

        const uint64_t offs = pos - cached_block;
        const uint64_t num_bytes = std::min(length, cached_length - offs);
        std::memcpy(write_buffer + offs, data, num_bytes);
        dirty = true;
        pos += num_bytes;
        data += num_bytes;
        length -= num_bytes;

        if (pos == cached_block + cached_length) flush();
    }
}

inline bool direct_extraction_file::writer::copy_if_large(uint64_t source, char* destination, uint64_t length)
{
    if (source > pos || length > pos - source)
        throw std::runtime_error("an extraction source has not been written yet");
    if (length == 0) return true;

    const bool in_write_block = cached_block != none && source >= cached_block
        && length <= pos - source;
    if (length < extract_direct_copy_min && !in_write_block) return false;

    uint64_t copied = 0;
    if (cached_block != none && source < cached_block) {
        copied = std::min(length, cached_block - source);
        read_written_range(source, destination, copied);
    } else if (cached_block == none) {
        read_written_range(source, destination, length);
        return true;
    }

    if (copied < length) {
        const uint64_t from = source + copied;
        if (cached_block == none || from < cached_block || length - copied > pos - from)
            throw std::runtime_error("an extraction source is missing from the output file");
        std::memcpy(destination + copied, write_buffer + (from - cached_block),
            length - copied);
    }
    return true;
}

inline void direct_extraction_file::writer::read_written_range(uint64_t source, char* destination, uint64_t length)
{
    // the previous writer may write the first sector of the interval concurrently; lock it while
    // reading from it
    const uint64_t first_sector = (begin / sector) * sector;
    const bool reads_shared_sector = begin_mutex != nullptr && begin % sector != 0
        && source < first_sector + sector && source + length > first_sector;
    std::unique_lock<std::mutex> guard;
    if (reads_shared_sector) guard = std::unique_lock<std::mutex>(*begin_mutex);
    file.read(source, destination, length, read_buffer);
}

inline void direct_extraction_file::writer::select_block()
{
    flush();
    const uint64_t selected = (pos / sector) * sector;
    std::mutex* mutex = nullptr;
    if (begin_mutex != nullptr && begin % sector != 0
            && selected == (begin / sector) * sector)
        mutex = begin_mutex;
    if (end_mutex != nullptr && end % sector != 0
            && selected == (end / sector) * sector)
        mutex = end_mutex;
    if (mutex != nullptr) sector_guard = std::unique_lock<std::mutex>(*mutex);
    cached_block = selected;

    // a sector that is not entirely inside the interval is buffered alone, else up to
    // write_block_length bytes of whole sectors of the interval
    const uint64_t aligned_end = (end / sector) * sector;
    cached_length = pos != selected || selected >= aligned_end
        ? sector : std::min(write_block_length, aligned_end - selected);

    // a sector that is shared with another writer or contains bytes before the interval is read
    // first, to preserve these bytes; a shared sector stays locked until it is written (see flush)
    if (mutex != nullptr || pos != selected) file.read_sector(selected, write_buffer);
    else if (cached_length == sector && end < selected + sector)
        std::memset(write_buffer, 0, sector);
}

inline void direct_extraction_file::writer::flush()
{
    if (!dirty) return;
    file.write_block(cached_block, write_buffer, cached_length);
    dirty = false;
    cached_block = none;
    cached_length = 0;
    if (sector_guard.owns_lock()) sector_guard.unlock();
}

inline bool direct_extraction_file::finish()
{
    if (fd < 0) return true;
    bool good = true;
    if (!platform::set_size(fd, logical_length)) good = false;
    if (!platform::sync_data(fd)) good = false;
    if (platform::close(fd) != 0) good = false;
    fd = -1;
    logical_length = physical_length = 0;
    return good;
}

inline void direct_extraction_file::read(uint64_t from, char* destination, uint64_t length, char* scratch)
{
    if (from > physical_length || length > physical_length - from)
        throw std::runtime_error("an extraction read is outside the output file");

    while (length > 0) {
        const uint64_t aligned_from = (from / sector) * sector;
        const uint64_t skip = from - aligned_from;
        const uint64_t num_bytes = std::min(length, read_buffer_length - skip);
        const uint64_t span = div_ceil<uint64_t>(skip + num_bytes, sector) * sector;
        read_all(scratch, span, aligned_from);
        std::memcpy(destination, scratch + skip, num_bytes);
        from += num_bytes;
        destination += num_bytes;
        length -= num_bytes;
    }
}

template <typename parsing_t>
inline void extract_range(const parsing_t& parsing, uint64_t from, uint64_t to, char* into)
{
    if constexpr (requires { parsing.extract(from, to - from + 1, into); }) {
        parsing.extract(from, to - from + 1, into);
    } else {
        uint64_t i = 0;
        parsing.for_each_in_range(int64_t(from), int64_t(to),
            [&](int64_t symbol) { into[i++] = char((uint8_t) symbol); });
    }
}

template <typename parsing_t>
inline void extract_phrases_to_file(const parsing_t& parsing, const std::vector<extract_range_t>& ranges,
    uint64_t num_symbols, const std::string& path, uint64_t p, phase_progress& progress)
{
    direct_extraction_file output;
    if (!output.open_file(path, num_symbols)) throw std::runtime_error("cannot create " + path);
    uint64_t output_pos = 0; // position in the output file of the current range

    // returns the maximum number of tiles of a range of length symbols starting at position
    auto max_tiles_of = [&](uint64_t position, uint64_t length) -> uint64_t {
        if (p <= 1 || length < extract_min_parallel_length) return 1;
        const uint64_t phrase_count = parsing.num_phrases_in(position, position + length - 1);
        return std::min({ p * extract_tiles_per_thread,
            std::max<uint64_t>(1, length / extract_min_tile_length),
            std::max<uint64_t>(1, phrase_count / extract_min_tile_phrases) });
    };

    for (uint64_t i = 0; i < ranges.size();) {
        // consecutive ranges with one tile are written by one writer, which writes whole blocks instead of the edge
        // sectors of each range
        uint64_t group_end = i;
        uint64_t group_length = 0;

        while (group_end < ranges.size()) {
            const auto& [position, length] = ranges[group_end];
            if (max_tiles_of(position, length) > 1) break;
            group_length += length;
            group_end++;
        }

        if (group_end > i) {
            direct_extraction_file::writer writer(output, output_pos, output_pos + group_length);
            uint64_t progress_pending = 0;

            for (; i < group_end; i++) {
                const auto& [position, length] = ranges[i];
                const uint64_t range_file = output_pos;

                auto copy_materialized = [&](uint64_t source, char* destination, uint64_t count) {
                    return writer.copy_if_large(range_file + (source - position), destination, count);
                };

                auto emit = [&](uint64_t, const char* phrase, uint64_t count) {
                    writer.write(phrase, count);
                    progress_pending += count;

                    if (progress_pending >= extract_progress_chunk) {
                        progress.advance(progress_pending);
                        progress_pending = 0;
                    }
                };

                parsing.extract_phrases(position, length, position, copy_materialized, emit);
                output_pos += length;
            }

            writer.finish();
            progress.advance(progress_pending);
            continue;
        }

        // plain variables, because the OpenMP region below captures them (clang cannot capture structured bindings)
        const uint64_t position = ranges[i].first;
        const uint64_t length = ranges[i].second;
        i++;
        const uint64_t max_tiles = max_tiles_of(position, length);
        std::vector<uint64_t> cuts { position };

        // cut the range into tiles of about the same length, at phrase boundaries; each tile has at least one sector
        for (uint64_t k = 1; k < max_tiles; k++) {
            const uint64_t quotient = length / max_tiles;
            const uint64_t remainder = length % max_tiles;
            const uint64_t target = position + quotient * k + std::min(k, remainder);
            const uint64_t boundary = parsing.phrase_end(target - 1) + 1;
            if (boundary < position + length && boundary > cuts.back()
                    && boundary - cuts.back() >= direct_extraction_file::sector)
                cuts.push_back(boundary);
        }

        // merge a last tile that is shorter than a sector into the previous tile
        if (cuts.size() > 1 && position + length - cuts.back() < direct_extraction_file::sector) cuts.pop_back();
        cuts.push_back(position + length);
        const uint64_t tiles = cuts.size() - 1;
        const uint64_t workers = std::min(p, tiles);
        std::vector<std::exception_ptr> failures(tiles);
        std::vector<std::mutex> sector_mutexes(tiles - 1);
        std::vector<uint8_t> completed(tiles, 0);
        std::atomic<uint64_t> next_tile { 0 };
        std::atomic<uint64_t> committed_file { output_pos };
        std::mutex commit_mutex;
        std::condition_variable committed_changed;
        uint64_t committed_tiles = 0; // protected by commit_mutex
        bool failed = false;           // protected by commit_mutex

        #pragma omp parallel num_threads(workers)
        {
            while (true) {
                const uint64_t k = next_tile.fetch_add(1, std::memory_order_relaxed);
                if (k >= tiles) break;

                // wait until tile k is one of the first 'workers' tiles after the completed prefix
                {
                    std::unique_lock<std::mutex> guard(commit_mutex);
                    committed_changed.wait(guard, [&] { return failed || k < committed_tiles + workers; });
                    if (failed) break;
                }

                try {
                    const uint64_t begin = cuts[k];
                    const uint64_t end = cuts[k + 1];
                    const uint64_t file_begin = output_pos + (begin - position);
                    direct_extraction_file::writer writer(output, file_begin, output_pos + (end - position),
                        k == 0 ? nullptr : &sector_mutexes[k - 1], k + 1 == tiles ? nullptr : &sector_mutexes[k]);
                    uint64_t progress_pending = 0;

                    auto copy_materialized = [&](uint64_t source, char* destination, uint64_t count) {
                        const uint64_t source_file = output_pos + (source - position);

                        // a source before the tile is copied only if it lies in the completed prefix (committed_file
                        // is rounded down to a multiple of a sector)
                        const uint64_t committed = committed_file.load(std::memory_order_acquire);
                        const bool globally_ready = source_file <= committed && count <= committed - source_file;
                        if (source < begin && !globally_ready) return false;
                        return writer.copy_if_large(source_file, destination, count);
                    };

                    auto emit = [&](uint64_t, const char* phrase, uint64_t count) {
                        writer.write(phrase, count);
                        progress_pending += count;

                        if (progress_pending >= extract_progress_chunk) {
                            progress.advance(progress_pending);
                            progress_pending = 0;
                        }
                    };

                    parsing.extract_phrases(begin, end - begin, position, copy_materialized, emit);
                    writer.finish();

                    // extend the completed prefix by all completed tiles that follow it
                    {
                        std::lock_guard<std::mutex> guard(commit_mutex);
                        completed[k] = 1;
                        while (committed_tiles < tiles && completed[committed_tiles]) committed_tiles++;
                        const uint64_t boundary_file = output_pos + (cuts[committed_tiles] - position);
                        const uint64_t stable_file = std::max(output_pos,
                            boundary_file / direct_extraction_file::sector * direct_extraction_file::sector);
                        committed_file.store(stable_file, std::memory_order_release);
                    }

                    committed_changed.notify_all();
                    progress.advance(progress_pending);
                } catch (...) {
                    failures[k] = std::current_exception();

                    {
                        std::lock_guard<std::mutex> guard(commit_mutex);
                        failed = true;
                    }

                    committed_changed.notify_all();
                }
            }
        }

        for (const std::exception_ptr& failure : failures)
            if (failure != nullptr) std::rethrow_exception(failure);

        output_pos += length;
    }

    if (!output.finish()) throw std::runtime_error("cannot write " + path);
}

template <typename parsing_t, typename write_fnc_t>
inline void extract_windows(const parsing_t& parsing, const std::vector<extract_range_t>& ranges, uint64_t p,
    phase_progress& progress, write_fnc_t write)
{
    std::string window; // the symbols of the current window

    for (const auto& [position, length] : ranges) {
        const uint64_t last = position + length - 1;

        for (uint64_t window_beg = position; window_beg <= last; window_beg += extract_window_length) {
            const uint64_t window_end = std::min(last, window_beg + extract_window_length - 1);
            const uint64_t window_length = window_end - window_beg + 1;
            resize_no_init(window, window_length);

            if (p <= 1 || window_length < extract_min_parallel_length) {
                extract_range(parsing, window_beg, window_end, window.data());
            } else {
                const uint64_t pieces = std::min<uint64_t>(p, div_ceil(window_length, extract_min_parallel_length));

                #pragma omp parallel for num_threads(pieces) schedule(static, 1)
                for (uint64_t k = 0; k < pieces; k++) {
                    const uint64_t begin = window_beg + window_length * k / pieces;
                    const uint64_t end = window_beg + window_length * (k + 1) / pieces;
                    if (begin >= end) continue;
                    extract_range(parsing, begin, end - 1, window.data() + (begin - window_beg));
                }
            }

            write(window.data(), window_length);
            progress.advance(window_length);
        }
    }
}

template <typename parsing_t, typename write_fnc_t>
inline void extract_windows_in_order(const parsing_t& parsing, const std::vector<extract_range_t>& ranges,
    const std::string& sibling_file, uint64_t p, phase_progress& progress, write_fnc_t write)
{
    if constexpr (!extracts_phrase_by_phrase<parsing_t>) {
        extract_windows(parsing, ranges, p, progress, write);
    } else {
        uint64_t num_symbols = 0;
        for (const extract_range_t& range : ranges) num_symbols += range.second;
        if (num_symbols == 0) return;
        temporary_files tmp_files;
        const std::string path = temporary_file_path("extracted-text-", sibling_file);
        tmp_files.paths = { path };
        extract_phrases_to_file(parsing, ranges, num_symbols, path, p, progress);
        direct_reader in(path);

        for (uint64_t window_beg = 0; window_beg < num_symbols; window_beg += extract_window_length) {
            const uint64_t window_end = std::min(num_symbols, window_beg + extract_window_length);
            write(reinterpret_cast<const char*>(in.read(window_beg, window_end)), window_end - window_beg);
        }
    }
}

template <typename parsing_t>
inline void extract_windows(const parsing_t& parsing, const std::vector<extract_range_t>& ranges, std::ostream& out,
    bool newline_after_range, uint64_t p, phase_progress& progress)
{
    for (const extract_range_t& range : ranges) {
        extract_windows(parsing, { range }, p, progress, [&](const char* symbols, uint64_t length) {
            out.write(symbols, std::streamsize(length));
            if (!out) throw std::runtime_error("cannot write the extracted text");
        });

        if (newline_after_range) out << '\n';
    }

    out.flush();
}

#endif // STRING_UTILS_MISC_EXTRACTION_TPP
