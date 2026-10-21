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

#ifndef STRING_UTILS_MISC_FILES_HPP
#define STRING_UTILS_MISC_FILES_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <iostream>
#include <fstream>
#include <climits>
#include <vector>
#include <omp.h>
#include <sdsl/int_vector_buffer.hpp>
#include <misc/log.hpp>
#include <misc/platform.hpp>
#include <misc/serialize.hpp>
#include <misc/utils.hpp>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <memory>
#include <unordered_map>

// ############################# PAGE CACHE #############################

/**
 * @brief waits until a file is written to disk (fdatasync) and drops it from the page cache; files smaller than
 *        8 MiB are skipped unless force is true (does nothing without platform::has_page_cache_control)
 * @param file path of the file
 * @param force true <=> the file is also dropped if it is smaller than 8 MiB
 */
inline void drop_from_page_cache(const std::string& file, bool force = false)
{
    // minimum size in bytes of a file that is dropped without force
    static constexpr uint64_t min_size = 8 * 1024 * 1024;

    if constexpr (!platform::has_page_cache_control) return;
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(file, ec);
    if (ec || (!force && size < min_size)) return;

    int fd = platform::open_file(file, platform::file_access::read, false);
    if (fd < 0) return;

    platform::sync_data(fd);
    platform::drop_cached(fd, 0, 0);
    platform::close(fd);
}

/**
 * @brief waits until a range of a file is written to disk and drops it from the page cache
 * @param fd file descriptor of the file
 * @param off offset of the first byte of the range
 * @param len length of the range in bytes
 */
inline void drop_range_from_page_cache(int fd, uint64_t off, uint64_t len)
{
    if (fd < 0 || len == 0) return;
    platform::write_back(fd, off, len);
    platform::drop_cached(fd, off, len);
}

/**
 * @brief advises the kernel not to keep the pages of a file in the page cache (POSIX_FADV_DONTNEED and
 *        POSIX_FADV_NOREUSE); used for files that are not opened with O_DIRECT
 * @param fd file descriptor of the file
 */
inline void advise_uncached(int fd)
{
    if (fd < 0) return;
    platform::drop_cached(fd, 0, 0);
    platform::advise_no_reuse(fd);
}

/**
 * @brief drops a set of files from the page cache periodically, with a background thread, until it is destroyed;
 *        a pass starts the writeback of each file and drops its pages that are already written to disk (without
 *        platform::has_page_cache_control, no thread is started and nothing is dropped)
 */
class page_cache_dropper {
    // ############################# DATA MEMBERS #############################

    std::vector<std::string> files_every_pass; // paths of the files dropped in every pass
    std::vector<std::string> files_at_end; // paths of the files dropped once, in the destructor
    std::vector<std::string> files_rarely; // paths of the files dropped in every rare_pass_interval-th pass
    uint64_t rare_pass_interval; // number of passes between two drops of the files in files_rarely
    uint64_t num_passes = 0; // number of passes done so far
    bool running = false; // false <=> the destructor has asked the background thread to stop
    std::mutex mutex; // protects running
    std::condition_variable wake; // wakes the background thread up when running becomes false
    std::thread worker; // the background thread

    // ############################# INTERNAL METHODS #############################

    /**
     * @brief starts the writeback of files and drops their pages that are already written to disk; the pages
     *        that are still dirty are dropped by a later pass
     * @param files paths of the files
     */
    static void drop_files(const std::vector<std::string>& files);

public:
    // ############################# CONSTRUCTORS #############################

    /**
     * @brief starts the background thread
     * @param files_every_pass files that are dropped in every pass
     * @param files_at_end files that are dropped once, in the destructor
     * @param interval time between two passes
     * @param files_rarely files that are dropped only in every rare_pass_interval-th pass (e.g., files that are
     *        read sequentially with read-ahead)
     * @param rare_pass_interval number of passes between two drops of the files in files_rarely
     */
    explicit page_cache_dropper(std::vector<std::string> files_every_pass, std::vector<std::string> files_at_end = {},
        std::chrono::milliseconds interval = std::chrono::milliseconds(200),
        std::vector<std::string> files_rarely = {}, uint64_t rare_pass_interval = 50);

    // stops the background thread and drops all files once more
    ~page_cache_dropper();

    page_cache_dropper(const page_cache_dropper&) = delete;
    page_cache_dropper& operator=(const page_cache_dropper&) = delete;
};

// ############################# O_DIRECT #############################

// alignment in bytes that O_DIRECT requires of each buffer address, file offset and length (a sector)
using platform::direct_io_alignment;

/**
 * @brief opens a file with O_DIRECT, or without it if the file system does not support O_DIRECT
 * @param path path of the file
 * @param access how the file is opened
 * @param direct is set to true <=> the file is opened with O_DIRECT
 * @return the file descriptor, or -1 if the file could not be opened
 */
inline int open_direct_or_buffered(const std::string& path, platform::file_access access, bool& direct)
{
    int fd = platform::open_file(path, access, true);
    direct = fd >= 0;
    if (!direct) fd = platform::open_file(path, access, false);
    return fd;
}

// allocates bytes bytes aligned to direct_io_alignment (to be freed with platform::free_aligned); throws
// std::bad_alloc on failure
inline void* allocate_aligned(uint64_t bytes)
{
    void* memory = platform::allocate_aligned(bytes, direct_io_alignment);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

/**
 * @brief reads up to size bytes at an offset of a file with pread, until the end of the file or an error
 * @param fd file descriptor of the file
 * @param data buffer of at least size bytes to read into
 * @param size number of bytes to read
 * @param off offset in the file of the first byte to read
 * @return the number of bytes read
 */
inline uint64_t pread_fully(int fd, void* data, uint64_t size, uint64_t off)
{
    uint64_t num_read = 0;

    while (num_read < size) {
        int64_t num_bytes = platform::read_at(fd, (char*) data + num_read, size - num_read, off + num_read);
        if (num_bytes <= 0) break;
        num_read += uint64_t(num_bytes);
    }

    return num_read;
}

/**
 * @brief writes size bytes at an offset of a file with pwrite
 * @param fd file descriptor of the file
 * @param data the bytes to write
 * @param size number of bytes to write
 * @param off offset in the file of the first byte to write
 * @return whether all bytes were written
 */
inline bool pwrite_fully(int fd, const char* data, uint64_t size, uint64_t off)
{
    uint64_t num_written = 0;

    while (num_written < size) {
        int64_t num_bytes = platform::write_at(fd, data + num_written, size - num_written, off + num_written);
        if (num_bytes <= 0) return false;
        num_written += uint64_t(num_bytes);
    }

    return true;
}

/**
 * @brief base class of direct_output_filebuf and direct_input_filebuf: a file opened with O_DIRECT and a buffer aligned
 *        to the sector size. If the file system does not support O_DIRECT, the file is accessed through the
 *        page cache and its pages are dropped from the page cache every drop_interval bytes.
 */
class direct_filebuf_base : public std::streambuf {
public:
    // ############################# CONSTANTS #############################

    // number of bytes that are read or written between two drops from the page cache (without O_DIRECT)
    static constexpr uint64_t drop_interval = 64 * 1024 * 1024;
    // alignment in bytes of buffer addresses, file offsets and lengths with O_DIRECT
    static constexpr uint64_t sector = direct_io_alignment;
    static constexpr uint64_t buffer_length = 1024 * 1024; // size of the buffer in bytes (a multiple of sector)

protected:
    // ############################# DATA MEMBERS #############################

    int fd = -1; // file descriptor (-1 if no file is open)
    bool direct = false; // true <=> the file is opened with O_DIRECT
    /* the pages of the bytes [0..dropped_end-1] of the file have been dropped from the page cache (writer: or
       their writeback has been started, see direct_output_filebuf::writeback_beg); only used without O_DIRECT */
    uint64_t dropped_end = 0;
    /* the buffer, capacity bytes, aligned to sector;
       writer: [pbase()..pptr()-1] stores the bytes [num_flushed..num_bytes_written()-1] of the file, which are not
       written to the file yet (see direct_output_filebuf);
       reader: [gptr()..egptr()-1] stores the next bytes to read, the bytes before file_pos (see direct_input_filebuf) */
    char* buffer = nullptr;
    uint64_t capacity = buffer_length; // size of the buffer in bytes (see direct_input_filebuf::read_at_most)

    // ############################# INTERNAL METHODS #############################

    // allocates the buffer, if it has not been allocated yet; throws std::bad_alloc if this fails
    void allocate_buffer() { if (buffer == nullptr) buffer = (char*) allocate_aligned(capacity); }

    // ############################# CONSTRUCTORS #############################

    ~direct_filebuf_base() override { if (buffer != nullptr) platform::free_aligned(buffer); }

public:
    // ############################# PUBLIC METHODS #############################

    // returns whether a file is open
    bool is_open() const { return fd >= 0; }
};

/**
 * @brief stream buffer that writes a file with O_DIRECT (see direct_filebuf_base); it does not support seeking.
 *        With O_DIRECT, only whole sectors are written while the file is open; the last bytes are written as a
 *        sector padded with zeros in close_file, after which the file is truncated to its length.
 */
class direct_output_filebuf : public direct_filebuf_base {
    // ############################# DATA MEMBERS #############################

    uint64_t file_length = 0; // length of the file in bytes (with O_DIRECT: set in close_file)
    // number of bytes passed to the file, i.e., the offset in the file of buffer[0] (with O_DIRECT a multiple of sector)
    uint64_t num_flushed = 0;
    // the range [writeback_beg..writeback_beg+writeback_len-1] of the file, whose writeback has been started (see write_all)
    uint64_t writeback_beg = 0;
    uint64_t writeback_len = 0;

    // ############################# INTERNAL METHODS #############################

    /**
     * @brief writes the buffered bytes to the file; with O_DIRECT, only whole sectors are written and the
     *        remaining bytes are moved to the start of the buffer
     * @return whether all bytes were written
     */
    bool flush_buffer();

    /**
     * @brief writes bytes at the current position of the file (without O_DIRECT) and drops the written
     *        pages from the page cache every drop_interval bytes
     * @param data the bytes to write
     * @param size number of bytes to write
     * @return whether all bytes were written
     */
    bool write_all(const char* data, uint64_t size);

protected:
    /**
     * @brief writes the full buffer to the file and puts a character into the buffer
     * @param c the character, or eof to only write the buffer
     * @return c, or eof if the file could not be written
     */
    int_type overflow(int_type c) override;

    int sync() override { return flush_buffer() ? 0 : -1; }

    // writes n bytes; without O_DIRECT, a write of at least buffer_length bytes bypasses the buffer
    std::streamsize xsputn(const char* s, std::streamsize n) override;

public:
    // ############################# CONSTRUCTORS #############################

    direct_output_filebuf() = default;

    // opens a file for writing (see open_file)
    explicit direct_output_filebuf(const std::string& path, bool append = false) { open_file(path, append); }

    ~direct_output_filebuf() override { close_file(); }

    direct_output_filebuf(const direct_output_filebuf&) = delete;
    direct_output_filebuf& operator=(const direct_output_filebuf&) = delete;

    // ############################# PUBLIC METHODS #############################

    // returns the number of bytes written to this buffer so far (including the bytes still in the buffer)
    uint64_t num_bytes_written() const { return num_flushed + uint64_t(pptr() - pbase()); }

    /**
     * @brief opens a file for writing; closes the currently open file
     * @param path path of the file
     * @param append true <=> the output is appended to the file (the file is then opened without O_DIRECT)
     */
    void open_file(const std::string& path, bool append = false);

    // writes the buffered bytes, drops the remaining pages of the file from the page cache and closes it
    bool close_file();
};

/** @brief output file stream that writes with O_DIRECT (see direct_output_filebuf) */
class direct_ofstream : public std::ostream {
    direct_output_filebuf buf;

public:
    direct_ofstream() : std::ostream(&buf) { setstate(std::ios_base::failbit); }

    // opens a file for writing (see open)
    explicit direct_ofstream(const std::string& path,
        std::ios_base::openmode mode = std::ios_base::out) : std::ostream(&buf)
    {
        open(path, mode);
    }

    direct_ofstream(const direct_ofstream&) = delete;
    direct_ofstream& operator=(const direct_ofstream&) = delete;
    direct_ofstream(direct_ofstream&&) = delete;
    direct_ofstream& operator=(direct_ofstream&&) = delete;

    /**
     * @brief opens a file for writing; closes the currently open file
     * @param path path of the file
     * @param mode open mode as for std::ofstream; only std::ios_base::app (append) is considered
     */
    void open(const std::string& path, std::ios_base::openmode mode = std::ios_base::out)
    {
        clear();
        buf.open_file(path, (mode & std::ios_base::app) != 0);
        if (!buf.is_open()) setstate(std::ios_base::failbit);
    }

    // writes the buffered bytes and closes the file
    void close() { if (!buf.close_file()) setstate(std::ios_base::failbit); }
    // returns whether a file is open
    bool is_open() const { return buf.is_open(); }

    // returns the number of bytes written so far (see direct_output_filebuf::num_bytes_written)
    uint64_t num_bytes_written() const { return buf.num_bytes_written(); }
};

/**
 * @brief stream buffer that reads a file with O_DIRECT (see direct_filebuf_base); with O_DIRECT, the buffer is
 *        filled with whole sectors, starting at the sector containing the read position
 */
class direct_input_filebuf : public direct_filebuf_base {
    // ############################# DATA MEMBERS #############################

    // offset in the file after the last byte read into the buffer
    uint64_t file_pos = 0;

    // ############################# INTERNAL METHODS #############################

    // drops the pages read since the last drop from the page cache, if they are at least drop_interval bytes
    void drop_read_pages();

protected:
    // fills the buffer from the file; returns the first character read, or eof at the end of the file
    int_type underflow() override;

    // reads n bytes; without O_DIRECT, a read of at least capacity bytes bypasses the buffer
    std::streamsize xsgetn(char* s, std::streamsize n) override;

    // sets the read position to pos; returns the new read position, or -1 if seeking failed
    pos_type seekpos(pos_type pos, std::ios_base::openmode which = std::ios_base::in) override
    {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

    /**
     * @brief moves the read position and empties the buffer
     * @param off offset to move the read position by
     * @param dir position the offset is relative to
     * @return the new read position, or -1 if seeking failed
     */
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
        std::ios_base::openmode = std::ios_base::in) override;

public:
    // ############################# CONSTRUCTORS #############################

    direct_input_filebuf() = default;

    // opens a file for reading (see open_file)
    explicit direct_input_filebuf(const std::string& path) { open_file(path); }

    ~direct_input_filebuf() override { close_file(); }

    direct_input_filebuf(const direct_input_filebuf&) = delete;
    direct_input_filebuf& operator=(const direct_input_filebuf&) = delete;

    // ############################# PUBLIC METHODS #############################

    // returns the current read position in the file, i.e., the number of bytes read from this buffer so far
    uint64_t read_pos() const { return file_pos - uint64_t(egptr() - gptr()); }

    /**
     * @brief limits the size of the buffer to the number of bytes that are read at most (plus 2 sectors); has
     *        to be called before the first file is opened
     * @param bytes maximum number of bytes that are read
     */
    void read_at_most(uint64_t bytes)
    {
        if (buffer != nullptr || bytes >= buffer_length) return;
        capacity = std::min<uint64_t>(buffer_length, (bytes / sector + 2) * sector);
    }

    // opens a file for reading; closes the currently open file
    void open_file(const std::string& path);

    // drops the pages read since the last drop from the page cache (without O_DIRECT) and closes the file
    void close_file();
};

/** @brief input file stream that reads with O_DIRECT (see direct_input_filebuf) */
class direct_ifstream : public std::istream {
    direct_input_filebuf buf;

public:
    direct_ifstream() : std::istream(&buf) { setstate(std::ios_base::failbit); }

    // opens a file for reading (see open)
    explicit direct_ifstream(const std::string& path,
        std::ios_base::openmode = std::ios_base::in) : std::istream(&buf)
    {
        open(path);
    }

    // opens a file for reading, of which at most read_at_most bytes are read (see direct_input_filebuf::read_at_most)
    direct_ifstream(const std::string& path, std::ios_base::openmode, uint64_t read_at_most) : std::istream(&buf)
    {
        buf.read_at_most(read_at_most);
        open(path);
    }

    direct_ifstream(const direct_ifstream&) = delete;
    direct_ifstream& operator=(const direct_ifstream&) = delete;
    direct_ifstream(direct_ifstream&&) = delete;
    direct_ifstream& operator=(direct_ifstream&&) = delete;

    // opens a file for reading; closes the currently open file; the open mode is ignored
    void open(const std::string& path, std::ios_base::openmode = std::ios_base::in)
    {
        clear();
        buf.open_file(path);
        if (!buf.is_open()) setstate(std::ios_base::failbit);
    }

    // closes the file
    void close() { buf.close_file(); }
    // returns whether a file is open
    bool is_open() const { return buf.is_open(); }

    // returns the current read position in the file (see direct_input_filebuf::read_pos)
    uint64_t read_pos() const { return buf.read_pos(); }
};

// block function of read_file_in_blocks that does nothing
inline constexpr auto ignore_block = [](const char*, uint64_t, uint64_t) {};

/**
 * @brief reads a file in blocks with O_DIRECT and calls a function for each block
 * @tparam fnc_t type of the block function
 * @param path path of the file
 * @param size number of bytes to read
 * @param into buffer of at least size bytes to read into, or nullptr to read each block into the same
 *        temporary buffer of block_size bytes
 * @param process function called as process(data, length, offset) for each block
 * @param block_size size of a block in bytes
 * @param prog counts the bytes read (see phase_progress)
 * @return whether all size bytes were read
 */
template <typename fnc_t = decltype(ignore_block)>
inline bool read_file_in_blocks(const std::string& path, uint64_t size, char* into = nullptr,
    fnc_t process = ignore_block, uint64_t block_size = 64 * 1024 * 1024,
    phase_progress* prog = nullptr)
{
    direct_ifstream in(path);
    if (!in.is_open()) return false;

    // the temporary buffer (if into is nullptr): min(block_size, max(size, 1)) bytes, the current block
    no_init_vector<char> block_buffer;
    if (into == nullptr) resize_no_init(block_buffer, std::min<uint64_t>(block_size, std::max<uint64_t>(size, 1)));
    const uint64_t block_len = into == nullptr ? block_buffer.size() : block_size;
    uint64_t pos = 0;

    while (pos < size) {
        char* data = into != nullptr ? into + pos : block_buffer.data();
        const uint64_t len = std::min<uint64_t>(block_len, size - pos);
        in.read(data, std::streamsize(len));
        if (!in.good()) return false;
        process(data, len, pos);
        pos += len;
        if (prog != nullptr) prog->advance(len);
    }

    return true;
}

/**
 * @brief reads a file with O_DIRECT, by position (operator[]) or by range (read()). If the file system does
 *        not support O_DIRECT, the file is read through the page cache and the read pages are dropped from it.
 *        A reader has one buffer, which stores the block or the range that was read last; several threads
 *        need one reader each.
 */
class direct_reader {
public:
    // ############################# CONSTANTS #############################

    // alignment in bytes of buffer addresses, file offsets and lengths with O_DIRECT
    static constexpr uint64_t sector = direct_io_alignment;
    static constexpr uint64_t block = 1024 * 1024; // number of bytes that operator[] reads at once

private:
    // ############################# DATA MEMBERS #############################

    int fd = -1; // file descriptor (-1 if no file is open)
    bool direct = false; // true <=> the file is opened with O_DIRECT
    /* the buffer, capacity bytes, aligned to sector; buffer[i] is the byte buffer_beg + i of the file for i in
       [0..buffer_end-buffer_beg-1] */
    uint8_t* buffer = nullptr;
    uint64_t capacity = 0; // size of the buffer in bytes
    uint64_t file_length = 0; // length of the file in bytes
    uint64_t buffer_beg = 0; // offset in the file of buffer[0]
    uint64_t buffer_end = 0; // offset in the file after the last byte in the buffer

    // ############################# INTERNAL METHODS #############################

    // allocates a buffer of the given size, aligned to sector, if the buffer is smaller; its content is lost
    void reserve(uint64_t bytes);

    // reads the range [beg, end) into the buffer; with O_DIRECT, all sectors overlapping the range are read
    void read_range_into_buffer(uint64_t beg, uint64_t end);

    // reads the block starting at a position into the buffer; with O_DIRECT, starting at the sector containing it
    void read_block_at(uint64_t pos);

public:
    // ############################# CONSTRUCTORS #############################

    direct_reader() = default;
    explicit direct_reader(const std::string& path) { open_file(path); }
    ~direct_reader() { close_file(); if (buffer != nullptr) platform::free_aligned(buffer); }

    direct_reader(const direct_reader&) = delete;
    direct_reader& operator=(const direct_reader&) = delete;

    direct_reader(direct_reader&& other) noexcept { *this = std::move(other); }
    direct_reader& operator=(direct_reader&& other) noexcept;

    // ############################# PUBLIC METHODS #############################

    // opens a file for reading; closes the currently open file; throws if the file cannot be opened
    void open_file(const std::string& path);

    // closes the file
    void close_file()
    {
        if (fd >= 0) platform::close(fd);
        fd = -1;
        buffer_beg = buffer_end = 0;
    }

    // same as close_file()
    void close() { close_file(); }

    // returns the length of the file in bytes
    inline uint64_t size() const { return file_length; }

    /**
     * @brief returns the byte at a position of the file; if the position is not in the buffer, the block of
     *        the file starting at the position is read (intended for ascending positions)
     * @param pos [0..size()-1] position in the file
     */
    inline uint8_t operator[](uint64_t pos)
    {
        if (pos < buffer_beg || pos >= buffer_end) [[unlikely]] read_block_at(pos);
        return buffer[pos - buffer_beg];
    }

    /**
     * @brief reads the range [beg, end) of the file
     * @param beg [0..size()-1] first position of the range
     * @param end [beg+1..size()] position after the last position of the range
     * @return pointer to the first byte of the range, valid until the next call of read() or operator[]
     */
    inline const uint8_t* read(uint64_t beg, uint64_t end)
    {
        if (beg < buffer_beg || end > buffer_end) read_range_into_buffer(beg, end);
        return buffer + (beg - buffer_beg);
    }
};

/**
 * @brief reads ranges of a file in blocks of at most block_bytes bytes, with O_DIRECT or, if the file system does not
 *        support O_DIRECT, through the page cache (then the read pages are dropped from it every drop_interval bytes).
 *        The buffer is allocated with platform::allocate_aligned_counted, which malloc_count counts. Several threads
 *        need one reader each.
 */
class direct_range_reader {
public:
    // alignment in bytes of buffer addresses, file offsets and lengths with O_DIRECT
    static constexpr uint64_t sector = direct_io_alignment;
    // number of bytes that are read between two drops from the page cache (without O_DIRECT)
    static constexpr uint64_t drop_interval = 8 * 1024 * 1024;

private:
    std::string path; // path of the file
    int fd = -1; // file descriptor (-1 if no file is open)
    bool direct = false; // true <=> the file is opened with O_DIRECT
    uint64_t block_bytes = 0; // maximum number of bytes passed to the function of for_each_block at once
    uint8_t* buffer = nullptr; // the buffer, block_bytes + sector bytes (rounded up to a multiple of sector)

public:
    direct_range_reader() = default;
    direct_range_reader(const direct_range_reader&) = delete;
    direct_range_reader& operator=(const direct_range_reader&) = delete;

    ~direct_range_reader()
    {
        if (buffer != nullptr) platform::free_aligned(buffer);
        if (fd >= 0) platform::close(fd);
    }

    // returns whether a file is open
    inline bool is_open() const { return fd >= 0; }

    /**
     * @brief opens a file and allocates the buffer; throws std::runtime_error if the file cannot be opened and
     *        std::bad_alloc if the buffer cannot be allocated
     * @param file_path path of the file
     * @param with_o_direct whether the file is opened with O_DIRECT (see open_direct_or_buffered)
     * @param max_block_bytes maximum number of bytes passed to the function of for_each_block at once
     */
    void open(const std::string& file_path, bool with_o_direct, uint64_t max_block_bytes)
    {
        path = file_path;
        direct = with_o_direct;
        block_bytes = max_block_bytes;
        fd = platform::open_file(path, platform::file_access::read, direct);
        if (fd < 0) throw std::runtime_error("cannot open " + path);
        buffer = (uint8_t*) platform::allocate_aligned_counted(div_ceil(block_bytes, sector) * sector + sector, sector);
        if (buffer == nullptr) throw std::bad_alloc();
    }

    /**
     * @brief reads the bytes [from..to-1] of the file and calls fnc(bytes, len) for each block of len <= block_bytes
     *        consecutive bytes, in ascending order; throws std::runtime_error if the file ends before position to
     * @param from first position of the range
     * @param to position after the last position of the range
     * @param fnc function called with a pointer to the bytes of a block and their number
     */
    template <typename fnc_t>
    void for_each_block(uint64_t from, uint64_t to, fnc_t fnc)
    {
        // without O_DIRECT, the pages of the bytes [from..dropped_to-1] have been dropped from the page cache
        uint64_t dropped_to = from;

        for (uint64_t pos = from; pos < to;) {
            // whole sectors are read, starting with the sector that contains pos
            const uint64_t read_beg = (pos / sector) * sector;
            const uint64_t skip = pos - read_beg; // number of bytes read before pos
            uint64_t len = std::min<uint64_t>(block_bytes, to - pos); // number of bytes passed to fnc
            // only the read of the last sector of the file returns fewer bytes
            const uint64_t num_read = pread_fully(fd, buffer, div_ceil(skip + len, sector) * sector, read_beg);
            if (num_read <= skip) [[unlikely]]
                throw std::runtime_error("cannot read " + path + " at " + std::to_string(pos));
            len = std::min(len, num_read - skip);

            if (!direct && read_beg + num_read - dropped_to >= drop_interval) {
                platform::drop_cached(fd, dropped_to, read_beg + num_read - dropped_to);
                dropped_to = read_beg + num_read;
            }

            fnc(buffer + skip, len);
            pos += len;
        }

        if (!direct && to > dropped_to) platform::drop_cached(fd, dropped_to, to - dropped_to);
    }
};

/**
 * @brief a file of a known length that several threads write at disjoint ranges, in any order, with O_DIRECT (if the
 *        file system does not support O_DIRECT, through the page cache, whose pages of the file are dropped when it is
 *        closed). The whole sectors of a write are written directly; the bytes of a sector that a write covers only
 *        partly are collected in memory until the sector is complete, and then written once. close_file writes the
 *        sectors that are still incomplete padded with zeros and truncates the file to its length. Every byte of the
 *        file is written at most once.
 */
class direct_positioned_file {
public:
    // alignment in bytes of buffer addresses, file offsets and lengths with O_DIRECT
    static constexpr uint64_t sector = direct_io_alignment;
    // size in bytes of the aligned buffer that write copies the sectors of unaligned data through
    static constexpr uint64_t copy_buffer_bytes = 1024 * 1024;

private:
    // a sector that has been written only partly
    struct partial_sector {
        char* bytes; // the sector (aligned to sector), zero where it has not been written
        uint64_t written; // the number of bytes written
    };

    std::string path; // path of the file
    int fd = -1; // file descriptor (-1 if no file is open)
    bool direct = false; // true <=> the file is opened with O_DIRECT
    uint64_t length = 0; // the length of the file in bytes
    std::mutex mutex; // protects partial
    std::unordered_map<uint64_t, partial_sector> partial; // the partly written sectors, by their offset in the file

    // writes size bytes at an offset (with O_DIRECT: aligned data, offset and size); throws if this fails
    void write_through(const char* data, uint64_t size, uint64_t offset);

    // adds the bytes data[0..end-beg-1] at the offsets [beg, end) to the partly written sectors and writes the
    // sectors that become complete
    void write_partial(const char* data, uint64_t beg, uint64_t end);

public:
    direct_positioned_file() = default;

    // creates a file of a given length (see open_file)
    direct_positioned_file(const std::string& path, uint64_t length) { open_file(path, length); }

    // closes the file (see close_file)
    ~direct_positioned_file() { close_file(); }

    direct_positioned_file(const direct_positioned_file&) = delete;
    direct_positioned_file& operator=(const direct_positioned_file&) = delete;

    /**
     * @brief creates (or truncates) a file and sets its length; closes the currently open file; throws
     *        std::runtime_error if this fails
     * @param file_path path of the file
     * @param file_length the length of the file in bytes
     */
    void open_file(const std::string& file_path, uint64_t file_length);

    // returns whether a file is open
    bool is_open() const { return fd >= 0; }

    // returns the length of the file in bytes
    uint64_t size() const { return length; }

    /**
     * @brief writes bytes at an offset of the file; can be called by several threads at once. The sectors the write
     *        covers completely are written without copying if data + (sector - offset % sector) % sector is aligned to
     *        sector (e.g., if data is aligned to sector and offset is a multiple of sector). Throws
     *        std::runtime_error if the write fails and std::invalid_argument if it ends after the file.
     * @param data the bytes
     * @param size the number of bytes
     * @param offset the offset in the file of the first byte
     */
    void write(const char* data, uint64_t size, uint64_t offset);

    // writes the partly written sectors, truncates the file to its length and closes it; returns whether this succeeded
    bool close_file();
};

#include "files.tpp"

#endif // STRING_UTILS_MISC_FILES_HPP
