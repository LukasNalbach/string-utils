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
 * @file extraction.hpp
 * @brief extraction of ranges of the text from a compressed representation of it with random access; used by the
 *        decompression of a compressor (see compressor_cli.hpp)
 */

#ifndef STRING_UTILS_MISC_EXTRACTION_HPP
#define STRING_UTILS_MISC_EXTRACTION_HPP

#include <omp.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <misc/files.hpp>
#include <misc/log.hpp>
#include <misc/platform.hpp>
#include <misc/temp_files.hpp>
#include <misc/utils.hpp>

// number of symbols that are extracted into memory and written at once (not used by the phrase-wise extraction)
inline constexpr uint64_t extract_window_length = 16 * 1024 * 1024;

// number of symbols after which a thread of the phrase-wise extraction counts its progress
inline constexpr uint64_t extract_progress_chunk = 1024 * 1024;

// minimum number of symbols that are extracted with more than one thread
inline constexpr uint64_t extract_min_parallel_length = 1 << 16;

// phrase-wise extraction: maximum number of tiles per thread, minimum length of a tile in symbols and in phrases
inline constexpr uint64_t extract_tiles_per_thread = 2;
inline constexpr uint64_t extract_min_tile_length = 1024 * 1024;
inline constexpr uint64_t extract_min_tile_phrases = 1024;

// phrase-wise extraction: minimum length in bytes of a phrase source that is read from the output file
inline constexpr uint64_t extract_direct_copy_min = 4 * 1024;

/**
 * @brief true <=> the parsing is extracted to a file phrase by phrase, reading the sources of the phrases from
 *        the output file (parsing_t::extracts_phrase_by_phrase is true)
 */
template <typename parsing_t>
inline constexpr bool extracts_phrase_by_phrase = [] {
    if constexpr (requires { parsing_t::extracts_phrase_by_phrase; })
        return bool(parsing_t::extracts_phrase_by_phrase);
    else return false;
}();

/**
 * @brief output file of the phrase-wise extraction, written and read with O_DIRECT (or through the page cache, if the
 *        file system does not support O_DIRECT); only whole sectors at aligned offsets are read and written. Several
 *        writers write disjoint intervals of the file in parallel and read already written ranges of it (the sources
 *        of the phrases).
 */
class direct_extraction_file {
public:
    static constexpr uint64_t sector = direct_io_alignment;
    static constexpr uint64_t write_block_length = 1024 * 1024; // size in bytes of the write buffer of a writer
    static constexpr uint64_t read_buffer_length = 64 * 1024; // size in bytes of the read buffer of a writer

    direct_extraction_file() = default;
    ~direct_extraction_file() { close_file(); }

    direct_extraction_file(const direct_extraction_file&) = delete;
    direct_extraction_file& operator=(const direct_extraction_file&) = delete;

    // creates the file with a length of length bytes (at least 1); returns whether this succeeded
    bool open_file(const std::string& path, uint64_t length);

    /** @brief writes an interval [begin, end) of the file from left to right */
    class writer {
    public:
        // begin_mutex and end_mutex are the mutexes for the sectors shared with the previous and the next
        // writer (nullptr if there is none)
        writer(direct_extraction_file& file, uint64_t begin, uint64_t end,
            std::mutex* begin_mutex = nullptr, std::mutex* end_mutex = nullptr);

        // does not write the buffered bytes (see finish())
        ~writer()
        {
            platform::free_aligned(write_buffer);
            platform::free_aligned(read_buffer);
        }

        writer(const writer&) = delete;
        writer& operator=(const writer&) = delete;

        // appends length bytes to the written part of the interval
        void write(const char* data, uint64_t length);

        /**
         * @brief copies an already written range of the file, if it has at least extract_direct_copy_min
         *        bytes or lies in the write buffer
         * @param source first position of the range in the file
         * @param destination buffer to copy the range to
         * @param length length of the range in bytes
         * @return whether the range was copied
         */
        bool copy_if_large(uint64_t source, char* destination, uint64_t length);

        // writes the buffered bytes to the file; throws if the interval has not been written completely
        void finish()
        {
            if (pos != end) throw std::runtime_error("extraction did not fill its file interval");
            flush();
        }

    private:
        // reads an already written range of the file into destination
        void read_written_range(uint64_t source, char* destination, uint64_t length);

        // loads the block of the file starting at the sector containing pos into the write buffer
        void select_block();

        // writes the write buffer to the file
        void flush();

        static constexpr uint64_t none = std::numeric_limits<uint64_t>::max();
        direct_extraction_file& file;
        uint64_t begin;
        uint64_t pos; // position of the next byte to write
        uint64_t end;
        std::mutex* begin_mutex;
        std::mutex* end_mutex;
        uint64_t cached_block = none; // position in the file of the block in the write buffer (none if it is empty)
        uint64_t cached_length = 0; // length in bytes of the block in the write buffer
        bool dirty = false; // true <=> the write buffer contains bytes that are not written to the file
        char* write_buffer = nullptr;
        char* read_buffer = nullptr;
        std::unique_lock<std::mutex> sector_guard; // lock of the shared sector in the write buffer
    };

    // truncates the file to its length and closes it; returns whether this succeeded
    bool finish();

private:
    friend class writer;

    // reads length bytes starting at position from into destination, using the aligned buffer scratch of
    // read_buffer_length bytes
    void read(uint64_t from, char* destination, uint64_t length, char* scratch);

    // reads the sector starting at position from (a multiple of sector) into destination
    void read_sector(uint64_t from, char* destination) { read_all(destination, sector, from); }

    // writes length bytes (a multiple of sector) of source to the file, starting at position to (a multiple of sector)
    void write_block(uint64_t to, const char* source, uint64_t length) { write_all(source, length, to); }

    // reads length bytes (a multiple of sector) starting at position from (a multiple of sector) into destination
    void read_all(char* destination, uint64_t length, uint64_t from)
    {
        if (pread_fully(fd, destination, length, from) != length)
            throw std::runtime_error("cannot read the output file of the extraction");
    }

    // writes length bytes (a multiple of sector) of source to the file, starting at position to (a multiple of sector)
    void write_all(const char* source, uint64_t length, uint64_t to)
    {
        if (!pwrite_fully(fd, source, length, to))
            throw std::runtime_error("cannot write the output file of the extraction");
    }

    void close_file() { if (fd >= 0) (void) finish(); }

    int fd = -1;
    uint64_t logical_length = 0; // length of the file in bytes
    uint64_t physical_length = 0; // logical_length, rounded up to a multiple of sector
};

/**
 * @brief extracts T[from..to] from a parsing, using either parsing.extract() or parsing.for_each_in_range()
 * @param parsing the parsing
 * @param from [0..n-1] first position to extract
 * @param to [from..n-1] last position to extract
 * @param into buffer of at least to - from + 1 bytes
 */
template <typename parsing_t>
inline void extract_range(const parsing_t& parsing, uint64_t from, uint64_t to, char* into);

// a range of the text to extract: (position, length)
using extract_range_t = std::pair<uint64_t, uint64_t>;

/**
 * @brief extracts ranges of the text phrase by phrase into a file (see parsing_t::extracts_phrase_by_phrase): each
 *        range is cut into tiles at phrase boundaries, which the threads extract from left to right; the source of a
 *        phrase is read from the output file if it has already been written (by the same thread, or in the completed
 *        prefix of the range)
 * @tparam parsing_t type of the parsing
 * @param parsing the parsing
 * @param ranges the ranges (position, length), with 1 <= length <= n - position
 * @param num_symbols the sum of the lengths of the ranges
 * @param path the path of the output file
 * @param p the number of threads
 * @param progress counts the extracted symbols
 */
template <typename parsing_t>
void extract_phrases_to_file(const parsing_t& parsing, const std::vector<extract_range_t>& ranges,
    uint64_t num_symbols, const std::string& path, uint64_t p, phase_progress& progress);

/**
 * @brief extracts ranges of the text window by window (extract_window_length symbols at a time; the threads extract
 *        disjoint pieces of a window in parallel) and passes the windows in the order of the text to a function
 * @tparam parsing_t type of the parsing
 * @param parsing the parsing
 * @param ranges the ranges (position, length), with 1 <= length <= n - position
 * @param p the number of threads
 * @param progress counts the extracted symbols
 * @param write called as write(symbols, length) for each window
 */
template <typename parsing_t, typename write_fnc_t>
void extract_windows(const parsing_t& parsing, const std::vector<extract_range_t>& ranges, uint64_t p,
    phase_progress& progress, write_fnc_t write);

/**
 * @brief extracts ranges of the text and passes them window by window in their order to a function (see
 *        extract_windows); a parsing that is extracted phrase by phrase (see extracts_phrase_by_phrase) is extracted
 *        into a temporary file first, which is then read window by window
 * @tparam parsing_t type of the parsing
 * @param parsing the parsing
 * @param ranges the ranges (position, length), with 1 <= length <= n - position
 * @param sibling_file the temporary file is created in the directory of this file (see temporary_file_path)
 * @param p the number of threads
 * @param progress counts the extracted symbols
 * @param write called as write(symbols, length) for each window
 */
template <typename parsing_t, typename write_fnc_t>
void extract_windows_in_order(const parsing_t& parsing, const std::vector<extract_range_t>& ranges,
    const std::string& sibling_file, uint64_t p, phase_progress& progress, write_fnc_t write);

/**
 * @brief extracts ranges of the text window by window (see above) and writes them to a stream
 * @tparam parsing_t type of the parsing
 * @param parsing the parsing
 * @param ranges the ranges (position, length), with 1 <= length <= n - position
 * @param out the stream the text is written to
 * @param newline_after_range whether a newline is written after each range
 * @param p the number of threads
 * @param progress counts the extracted symbols
 */
template <typename parsing_t>
void extract_windows(const parsing_t& parsing, const std::vector<extract_range_t>& ranges, std::ostream& out,
    bool newline_after_range, uint64_t p, phase_progress& progress);

#include <misc/extraction.tpp>

#endif // STRING_UTILS_MISC_EXTRACTION_HPP
