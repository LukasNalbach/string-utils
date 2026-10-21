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

#ifndef STRING_UTILS_MISC_FILES_TPP
#define STRING_UTILS_MISC_FILES_TPP

#include <misc/files.hpp>

// ############################# page_cache_dropper #############################

inline page_cache_dropper::page_cache_dropper(std::vector<std::string> files_every_pass,
    std::vector<std::string> files_at_end, std::chrono::milliseconds interval,
    std::vector<std::string> files_rarely, uint64_t rare_pass_interval)
    : files_every_pass(std::move(files_every_pass)), files_at_end(std::move(files_at_end)),
      files_rarely(std::move(files_rarely)), rare_pass_interval(std::max<uint64_t>(1, rare_pass_interval))
{
    if constexpr (!platform::has_page_cache_control) return;
    running = true;

    worker = std::thread([this, interval]() {
        std::unique_lock<std::mutex> lock(mutex);

        while (running) {
            drop_files(this->files_every_pass);
            if (++num_passes % this->rare_pass_interval == 0) drop_files(this->files_rarely);
            // wait for the next pass, or until the destructor wakes the thread up
            wake.wait_for(lock, interval, [this]() { return !running; });
        }
    });
}

inline page_cache_dropper::~page_cache_dropper()
{
    {
        std::lock_guard<std::mutex> guard(mutex);
        running = false;
    }

    wake.notify_all();
    if (worker.joinable()) worker.join();
    drop_files(files_every_pass);
    drop_files(files_rarely);
    for (const std::string& file : files_at_end) drop_from_page_cache(file, true);
}

inline void page_cache_dropper::drop_files(const std::vector<std::string>& files)
{
    if constexpr (!platform::has_page_cache_control) return;

    for (const std::string& file : files) {
        int fd = platform::open_file(file, platform::file_access::read, false);
        if (fd < 0) continue;
        platform::start_writeback(fd, 0, 0);
        platform::drop_cached(fd, 0, 0);
        platform::close(fd);
    }
}

// ############################# direct_output_filebuf #############################

inline void direct_output_filebuf::open_file(const std::string& path, bool append)
{
    close_file();
    allocate_buffer();

    if (append) {
        fd = platform::open_file(path, platform::file_access::append, false);
        direct = false;
    } else {
        fd = open_direct_or_buffered(path, platform::file_access::write, direct);
    }

    if (!direct) advise_uncached(fd);
    // the bytes already in the file count as written and dropped
    file_length = append && fd >= 0 ? uint64_t(std::max<platform::file_offset_t>(0, platform::seek_end(fd))) : 0;
    num_flushed = file_length;
    dropped_end = file_length;
    writeback_beg = 0;
    writeback_len = 0;
    setp(buffer, buffer + buffer_length);
}

inline bool direct_output_filebuf::close_file()
{
    if (fd < 0) return true;
    bool ok = flush_buffer();

    if (direct) {
        // write the remaining bytes as a sector padded with zeros and truncate the file to its length
        uint64_t rest = uint64_t(pptr() - pbase());
        file_length = num_flushed + rest;

        // if flush_buffer failed, more than a sector can be left in the buffer
        if (ok && rest > 0) {
            std::memset(buffer + rest, 0, size_t(sector - rest));
            ok = pwrite_fully(fd, buffer, sector, num_flushed);
        }

        ok = platform::set_size(fd, file_length) && ok;
    } else {
        drop_range_from_page_cache(fd, writeback_beg, writeback_len);
        drop_range_from_page_cache(fd, dropped_end, file_length - dropped_end);
    }

    setp(buffer, buffer + buffer_length);
    ok = platform::close(fd) == 0 && ok;
    fd = -1;
    return ok;
}

inline direct_output_filebuf::int_type direct_output_filebuf::overflow(int_type c)
{
    if (!flush_buffer()) return traits_type::eof();
    if (c != traits_type::eof()) { *pptr() = traits_type::to_char_type(c); pbump(1); }
    return c;
}

inline std::streamsize direct_output_filebuf::xsputn(const char* s, std::streamsize n)
{
    // with O_DIRECT, all bytes are written through the aligned buffer
    if (direct || n < std::streamsize(buffer_length)) return std::streambuf::xsputn(s, n);
    if (!flush_buffer()) return 0;
    return write_all(s, uint64_t(n)) ? n : 0;
}

inline bool direct_output_filebuf::flush_buffer()
{
    uint64_t num_bytes = uint64_t(pptr() - pbase());
    if (num_bytes == 0) return true;

    if (!direct) {
        setp(buffer, buffer + buffer_length);
        return write_all(buffer, num_bytes);
    }

    uint64_t num_sector_bytes = (num_bytes / sector) * sector; // number of bytes in whole sectors
    uint64_t rest = num_bytes - num_sector_bytes;

    if (num_sector_bytes > 0 && !pwrite_fully(fd, buffer, num_sector_bytes, num_flushed)) return false;
    num_flushed += num_sector_bytes;
    if (rest > 0) std::memmove(buffer, buffer + num_sector_bytes, size_t(rest));

    setp(buffer, buffer + buffer_length);
    pbump(int(rest));
    return true;
}

inline bool direct_output_filebuf::write_all(const char* data, uint64_t size)
{
    uint64_t num_written = 0;

    while (num_written < size) {
        int64_t num_bytes = platform::write_next(fd, data + num_written, size - num_written, file_length);
        if (num_bytes <= 0) return false;
        num_written += uint64_t(num_bytes);
        file_length += uint64_t(num_bytes);
        num_flushed = file_length;
    }

    if (file_length - dropped_end >= drop_interval) {
        // start the writeback of the range [dropped_end, file_length) without waiting for it and drop the
        // previous range, whose writeback has been started before
        platform::start_writeback(fd, dropped_end, file_length - dropped_end);
        drop_range_from_page_cache(fd, writeback_beg, writeback_len);
        writeback_beg = dropped_end;
        writeback_len = file_length - dropped_end;
        dropped_end = file_length;
    }

    return true;
}

// ############################# direct_input_filebuf #############################

inline void direct_input_filebuf::open_file(const std::string& path)
{
    close_file();
    allocate_buffer();
    fd = open_direct_or_buffered(path, platform::file_access::read, direct);
    dropped_end = 0;
    if (!direct) advise_uncached(fd);
    file_pos = 0;
    setg(buffer, buffer, buffer);
}

inline void direct_input_filebuf::close_file()
{
    if (fd < 0) return;
    if (!direct) platform::drop_cached(fd, dropped_end, file_pos - dropped_end);
    platform::close(fd);
    fd = -1;
}

inline direct_input_filebuf::int_type direct_input_filebuf::underflow()
{
    if (fd < 0) return traits_type::eof();

    if (!direct) {
        int64_t num_bytes = platform::read_next(fd, buffer, capacity, file_pos);
        if (num_bytes <= 0) return traits_type::eof();

        file_pos += uint64_t(num_bytes);
        setg(buffer, buffer, buffer + num_bytes);
        drop_read_pages();
        return traits_type::to_int_type(buffer[0]);
    }

    // read whole sectors, starting at the sector containing the read position, and skip the bytes before it
    uint64_t sector_beg = (file_pos / sector) * sector;
    uint64_t skip = file_pos - sector_beg;
    uint64_t num_read = pread_fully(fd, buffer, capacity, sector_beg);
    if (num_read <= skip) return traits_type::eof();

    file_pos = sector_beg + num_read;
    setg(buffer, buffer + skip, buffer + num_read);
    return traits_type::to_int_type(buffer[skip]);
}

inline std::streamsize direct_input_filebuf::xsgetn(char* s, std::streamsize n)
{
    std::streamsize from_buffer = std::min<std::streamsize>(n, egptr() - gptr());

    if (from_buffer > 0) {
        std::memcpy(s, gptr(), size_t(from_buffer));
        gbump(int(from_buffer));
    }

    std::streamsize left = n - from_buffer;
    // with O_DIRECT, all bytes are read through the aligned buffer
    if (direct || left < std::streamsize(capacity))
        return from_buffer + std::streambuf::xsgetn(s + from_buffer, left);

    uint64_t num_read = 0;

    while (num_read < uint64_t(left)) {
        int64_t num_bytes = platform::read_next(fd, s + from_buffer + num_read, uint64_t(left) - num_read, file_pos);
        if (num_bytes <= 0) break;
        num_read += uint64_t(num_bytes);
        file_pos += uint64_t(num_bytes);
        drop_read_pages();
    }

    return from_buffer + std::streamsize(num_read);
}

inline direct_input_filebuf::pos_type direct_input_filebuf::seekoff(off_type off, std::ios_base::seekdir dir,
    std::ios_base::openmode)
{
    if (fd < 0) return pos_type(off_type(-1));

    platform::file_offset_t new_pos;

    if (dir == std::ios_base::cur) {
        // the current read position is computed from file_pos (with O_DIRECT, the reads do not move the
        // position of the descriptor)
        new_pos = off_type(file_pos - uint64_t(egptr() - gptr())) + off;
    } else if (dir == std::ios_base::beg) {
        new_pos = platform::file_offset_t(off);
    } else {
        platform::file_offset_t end = platform::seek_end(fd);
        if (end < 0) return pos_type(off_type(-1));
        new_pos = end + off;
    }

    if (new_pos < 0) return pos_type(off_type(-1));
    // also move the position of the descriptor, which the reads without O_DIRECT use
    if (!platform::seek(fd, uint64_t(new_pos))) return pos_type(off_type(-1));

    file_pos = uint64_t(new_pos);
    dropped_end = std::min(dropped_end, file_pos);
    setg(buffer, buffer, buffer);
    return pos_type(off_type(new_pos));
}

inline void direct_input_filebuf::drop_read_pages()
{
    if (file_pos - dropped_end < drop_interval) return;
    platform::drop_cached(fd, dropped_end, file_pos - dropped_end);
    dropped_end = file_pos;
}

// ############################# direct_reader #############################

inline direct_reader& direct_reader::operator=(direct_reader&& other) noexcept
{
    if (this == &other) return *this;
    close_file();
    if (buffer != nullptr) platform::free_aligned(buffer);
    fd = other.fd;
    other.fd = -1;
    direct = other.direct;
    buffer = other.buffer;
    other.buffer = nullptr;
    capacity = other.capacity;
    other.capacity = 0;
    file_length = other.file_length;
    buffer_beg = other.buffer_beg;
    buffer_end = other.buffer_end;
    other.buffer_beg = other.buffer_end = 0;
    return *this;
}

inline void direct_reader::open_file(const std::string& path)
{
    close_file();
    fd = open_direct_or_buffered(path, platform::file_access::read, direct);
    if (fd < 0) throw std::runtime_error("cannot read " + path);
    if (!direct) advise_uncached(fd);

    std::error_code ec;
    file_length = std::filesystem::file_size(path, ec);
    if (ec) file_length = 0;

    reserve(block + sector);
    buffer_beg = buffer_end = 0;
}

inline void direct_reader::reserve(uint64_t bytes)
{
    if (capacity >= bytes) return;

    if (buffer != nullptr) {
        platform::free_aligned(buffer);
        buffer = nullptr;
        capacity = 0;
    }

    buffer = (uint8_t*) allocate_aligned(bytes);
    capacity = bytes;
}

inline void direct_reader::read_range_into_buffer(uint64_t beg, uint64_t end)
{
    if (fd < 0 || end > file_length) throw std::runtime_error("the file ends before the range does");
    // the range [read_beg, read_end) of the file that is read
    const uint64_t read_beg = direct ? (beg / sector) * sector : beg;
    const uint64_t read_end = direct ? ((end + sector - 1) / sector) * sector : end;
    const uint64_t read_len = read_end - read_beg;
    reserve(read_len);
    uint64_t num_read = 0;

    while (num_read < read_len && read_beg + num_read < file_length) {
        int64_t num_bytes = platform::read_at(fd, buffer + num_read, read_len - num_read, read_beg + num_read);
        if (num_bytes < 0) throw std::runtime_error("a read of a file range failed");
        if (num_bytes == 0) break;
        num_read += uint64_t(num_bytes);
        if (direct && num_read % sector != 0 && read_beg + num_read < file_length)
            throw std::runtime_error("a read of a file range stopped inside a sector");
    }

    if (!direct) platform::drop_cached(fd, read_beg, num_read);
    buffer_beg = read_beg;
    buffer_end = read_beg + num_read;
    if (buffer_end < end) throw std::runtime_error("the file ends before the range does");
}

inline void direct_reader::read_block_at(uint64_t pos)
{
    if (buffer == nullptr || fd < 0) { buffer_beg = buffer_end = 0; return; }
    reserve(block + sector);
    uint64_t read_beg = direct ? (pos / sector) * sector : pos; // offset in the file of the first byte read
    uint64_t bytes_to_read = std::min(block, file_length > read_beg ? file_length - read_beg : uint64_t(0));
    // number of bytes requested from the file (with O_DIRECT, rounded up to whole sectors)
    uint64_t read_len = direct ? ((bytes_to_read + sector - 1) / sector) * sector : bytes_to_read;
    uint64_t num_read = pread_fully(fd, buffer, read_len, read_beg);

    if (!direct) platform::drop_cached(fd, read_beg, num_read);
    buffer_beg = read_beg;
    buffer_end = read_beg + std::min(num_read, bytes_to_read);
    if (pos < file_length && buffer_end <= pos) throw std::runtime_error("direct_reader: cannot read the file");
}

// ############################# direct_positioned_file #############################

inline void direct_positioned_file::open_file(const std::string& file_path, uint64_t file_length)
{
    close_file();
    path = file_path;
    length = file_length;
    fd = open_direct_or_buffered(path, platform::file_access::write, direct);
    if (fd < 0) throw std::runtime_error("cannot create " + path);
    if (!direct) advise_uncached(fd);
    // the threads write far apart (see platform::make_sparse)
    platform::make_sparse(fd);
    if (!platform::set_size(fd, length)) throw std::runtime_error("cannot resize " + path);
}

inline void direct_positioned_file::write_through(const char* data, uint64_t size, uint64_t offset)
{
    if (!pwrite_fully(fd, data, size, offset)) throw std::runtime_error("cannot write " + path);
}

inline void direct_positioned_file::write_partial(const char* data, uint64_t beg, uint64_t end)
{
    std::lock_guard<std::mutex> guard(mutex);

    while (beg < end) {
        const uint64_t sector_beg = (beg / sector) * sector;
        const uint64_t num = std::min(end, sector_beg + sector) - beg;
        auto it = partial.find(sector_beg);

        if (it == partial.end()) {
            char* bytes = (char*) allocate_aligned(sector);
            std::memset(bytes, 0, sector);
            it = partial.emplace(sector_beg, partial_sector { bytes, 0 }).first;
        }

        std::memcpy(it->second.bytes + (beg - sector_beg), data, num);
        it->second.written += num;

        // the sector is complete once its bytes up to the end of the file are written
        if (it->second.written == std::min(sector, length - sector_beg)) {
            write_through(it->second.bytes, sector, sector_beg);
            platform::free_aligned(it->second.bytes);
            partial.erase(it);
        }

        data += num;
        beg += num;
    }
}

inline void direct_positioned_file::write(const char* data, uint64_t size, uint64_t offset)
{
    if (size == 0) return;
    if (offset > length || size > length - offset) throw std::invalid_argument("a write ends after " + path);
    const uint64_t end = offset + size;

    if (!direct) {
        write_through(data, size, offset);
        return;
    }

    // [first, last) are the sectors the write covers completely
    const uint64_t first = std::min(end, div_ceil(offset, sector) * sector);
    const uint64_t last = std::max(first, (end / sector) * sector);
    if (offset < first) write_partial(data, offset, first);
    if (last < end) write_partial(data + (last - offset), last, end);
    if (first == last) return;
    const char* sectors = data + (first - offset); // the bytes of the complete sectors

    if (uintptr_t(sectors) % sector == 0) {
        write_through(sectors, last - first, first);
        return;
    }

    std::unique_ptr<char, decltype(&platform::free_aligned)> buffer(
        (char*) allocate_aligned(std::min(copy_buffer_bytes, last - first)), &platform::free_aligned);

    for (uint64_t pos = first; pos < last; pos += copy_buffer_bytes) {
        const uint64_t num = std::min(copy_buffer_bytes, last - pos);
        std::memcpy(buffer.get(), sectors + (pos - first), num);
        write_through(buffer.get(), num, pos);
    }
}

inline bool direct_positioned_file::close_file()
{
    if (fd < 0) return true;
    bool ok = true;

    for (auto& [sector_beg, s] : partial) {
        ok = pwrite_fully(fd, s.bytes, sector, sector_beg) && ok;
        platform::free_aligned(s.bytes);
    }

    partial.clear();
    ok = platform::set_size(fd, length) && ok;
    if (!direct) drop_range_from_page_cache(fd, 0, length);
    ok = platform::close(fd) == 0 && ok;
    fd = -1;
    return ok;
}

#endif // STRING_UTILS_MISC_FILES_TPP
