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
 * @file platform.hpp
 * @brief the calls to the operating system: files (unbuffered and positioned I/O, page cache), CPUs (affinity,
 *        cores), memory (alignment, huge pages), console and environment. The Linux functions are defined here; the
 *        Windows functions that need <windows.h> are defined in src/platform_windows.cpp (the library
 *        string_utils_platform), so that no header includes <windows.h>.
 *
 * A file is identified by a descriptor (int) on both systems. On Windows it is a descriptor of the C runtime around a
 * handle opened with FILE_FLAG_OVERLAPPED, such that positioned reads and writes of several threads through the same
 * descriptor run in parallel (as pread and pwrite on Linux); such a handle has no file position, so the sequential
 * functions (read_next, write_next) take the position the caller tracks.
 */

#ifndef STRING_UTILS_MISC_PLATFORM_HPP
#define STRING_UTILS_MISC_PLATFORM_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <malloc.h>
#else
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

// the prefix of the environment variables read by string-utils (<prefix>_TMPDIR, <prefix>_HUGE_PAGES,
// <prefix>_TEST_SECONDS, ...); the build sets it to STRING_UTILS_PREFIX (see cmake/string_utils.cmake)
#ifndef STRING_UTILS_ENV_PREFIX
#define STRING_UTILS_ENV_PREFIX "STRING_UTILS"
#endif

namespace platform {

// ############################# FILES #############################

// how open_file opens a file
enum class file_access : uint8_t {
    read, // reads an existing file
    write, // creates a file or truncates an existing one, and writes it
    append, // creates a file or opens an existing one, and appends to it
    read_write // creates a file or truncates an existing one, and reads and writes it
};

/* alignment in bytes that unbuffered I/O (O_DIRECT, FILE_FLAG_NO_BUFFERING) requires of each buffer address, file
   offset and length: one sector of 4096 bytes, a multiple of the sector size of drives with 512-byte and 4096-byte
   sectors */
inline constexpr uint64_t direct_io_alignment = 4096;

#ifdef _WIN32
using file_offset_t = int64_t; // type of an offset in a file
#else
using file_offset_t = off_t; // type of an offset in a file
#endif

// largest offset in a file
inline constexpr uint64_t max_file_offset = uint64_t(std::numeric_limits<file_offset_t>::max());

#ifdef _WIN32
// true <=> the pages of a file in the page cache can be written back and dropped (drop_cached, start_writeback)
inline constexpr bool has_page_cache_control = false;

/**
 * @brief opens a file; with direct, the file is read and written unbuffered (FILE_FLAG_NO_BUFFERING), which fails if
 *        the sector size of the volume does not divide direct_io_alignment
 * @param path path of the file (UTF-8)
 * @param access how the file is opened
 * @param direct true <=> the file is opened for unbuffered I/O
 * @return the descriptor, or -1 if the file cannot be opened
 */
int open_file(const std::string& path, file_access access, bool direct);

/**
 * @brief opens a file for reading such that reopen_path(fd, path) names the opened file as long as the descriptor is
 *        open; the file can be renamed meanwhile (reopen_path returns the new path), but not written
 * @param path path of the file (UTF-8)
 * @return the descriptor, or -1 if the file cannot be opened
 */
int open_pinned(const std::string& path);

// returns a path that opens the file of a descriptor returned by open_pinned(path): its current path (path if that
// cannot be determined)
std::string reopen_path(int fd, const std::string& path);

// closes a descriptor; returns 0 on success
inline int close(int fd) { return _close(fd); }

/**
 * @brief reads up to size bytes at an offset of a file
 * @param fd the descriptor
 * @param data buffer of at least size bytes
 * @param size number of bytes to read
 * @param off offset in the file of the first byte to read
 * @return the number of bytes read (fewer only at the end of the file, 0 after it), or -1 on an error
 */
int64_t read_at(int fd, void* data, uint64_t size, uint64_t off);

/**
 * @brief writes up to size bytes at an offset of a file
 * @param fd the descriptor
 * @param data the bytes to write
 * @param size number of bytes to write
 * @param off offset in the file of the first byte to write
 * @return the number of bytes written, or -1 on an error
 */
int64_t write_at(int fd, const void* data, uint64_t size, uint64_t off);

// reads up to size bytes at the position pos of the file (see read_at), where the previous sequential read ended
inline int64_t read_next(int fd, void* data, uint64_t size, uint64_t pos) { return read_at(fd, data, size, pos); }

// writes up to size bytes at the position pos of the file (see write_at), where the previous sequential write ended
inline int64_t write_next(int fd, const void* data, uint64_t size, uint64_t pos)
{
    return write_at(fd, data, size, pos);
}

// sets the position of the sequential reads and writes to pos; returns whether this succeeded
inline bool seek(int, uint64_t) { return true; }

// returns the length of the file in bytes, or -1 on an error
file_offset_t seek_end(int fd);

// sets the length of the file to size bytes (truncates or extends it); returns whether this succeeded
bool set_size(int fd, uint64_t size);

// waits until the written data of the file is on the disk; Windows keeps unbuffered and buffered I/O coherent
inline bool sync_data(int) { return true; }

/* marks the file as sparse: extending the file and writing far behind the written data does not write zeros into the
   gap first (on NTFS, a write behind the valid data length fills the gap with zeros) */
void make_sparse(int fd);

// drops the pages of the bytes [off..off+len-1] (len = 0: up to the end) of the file from the page cache
inline void drop_cached(int, uint64_t, uint64_t) {}

// advises the kernel that the pages of the file are not accessed again
inline void advise_no_reuse(int) {}

// starts the writeback of the bytes [off..off+len-1] (len = 0: up to the end) of the file without waiting for it
inline void start_writeback(int, uint64_t, uint64_t) {}

// writes the bytes [off..off+len-1] of the file back to the disk and waits until they are written
inline void write_back(int, uint64_t, uint64_t) {}
#else
// true <=> the pages of a file in the page cache can be written back and dropped (drop_cached, start_writeback)
inline constexpr bool has_page_cache_control = true;

// permissions of a file created by open_file: rw-r--r-- (reduced by the umask)
inline constexpr mode_t created_file_mode = 0644;

/**
 * @brief opens a file; with direct, the file is read and written with O_DIRECT, which fails if the file system does
 *        not support it
 * @param path path of the file
 * @param access how the file is opened
 * @param direct true <=> the file is opened with O_DIRECT
 * @return the descriptor, or -1 if the file cannot be opened
 */
inline int open_file(const std::string& path, file_access access, bool direct)
{
    int flags = O_RDONLY;
    if (access == file_access::write) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (access == file_access::append) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (access == file_access::read_write) flags = O_RDWR | O_CREAT | O_TRUNC;
    if (direct) flags |= O_DIRECT;
    return ::open(path.c_str(), flags, created_file_mode);
}

/**
 * @brief opens a file for reading such that reopen_path(fd, path) names the opened file as long as the descriptor is
 *        open, even if the file is deleted or replaced meanwhile
 * @param path path of the file
 * @return the descriptor, or -1 if the file cannot be opened
 */
inline int open_pinned(const std::string& path) { return ::open(path.c_str(), O_RDONLY); }

// returns a path that opens the file of a descriptor returned by open_pinned(path)
inline std::string reopen_path(int fd, const std::string&) { return "/proc/self/fd/" + std::to_string(fd); }

// closes a descriptor; returns 0 on success
inline int close(int fd) { return ::close(fd); }

/**
 * @brief reads up to size bytes at an offset of a file (pread)
 * @param fd the descriptor
 * @param data buffer of at least size bytes
 * @param size number of bytes to read
 * @param off offset in the file of the first byte to read
 * @return the number of bytes read (0 at the end of the file), or -1 on an error
 */
inline int64_t read_at(int fd, void* data, uint64_t size, uint64_t off)
{
    return ::pread(fd, data, size_t(size), off_t(off));
}

/**
 * @brief writes up to size bytes at an offset of a file (pwrite)
 * @param fd the descriptor
 * @param data the bytes to write
 * @param size number of bytes to write
 * @param off offset in the file of the first byte to write
 * @return the number of bytes written, or -1 on an error
 */
inline int64_t write_at(int fd, const void* data, uint64_t size, uint64_t off)
{
    return ::pwrite(fd, data, size_t(size), off_t(off));
}

// reads up to size bytes at the position of the descriptor, which is pos (see read_at)
inline int64_t read_next(int fd, void* data, uint64_t size, uint64_t) { return ::read(fd, data, size_t(size)); }

// writes up to size bytes at the position of the descriptor, which is pos (see write_at)
inline int64_t write_next(int fd, const void* data, uint64_t size, uint64_t) { return ::write(fd, data, size_t(size)); }

// sets the position of the descriptor to pos; returns whether this succeeded
inline bool seek(int fd, uint64_t pos) { return lseek(fd, off_t(pos), SEEK_SET) >= 0; }

// sets the position of the descriptor to the end of the file and returns it (the length), or -1 on an error
inline file_offset_t seek_end(int fd) { return lseek(fd, 0, SEEK_END); }

// sets the length of the file to size bytes (truncates or extends it); returns whether this succeeded
inline bool set_size(int fd, uint64_t size) { return ::ftruncate(fd, off_t(size)) == 0; }

// waits until the written data of the file is on the disk (fdatasync); returns whether this succeeded
inline bool sync_data(int fd) { return ::fdatasync(fd) == 0; }

// marks the file as sparse (files on Linux are sparse: extending a file allocates no blocks)
inline void make_sparse(int) {}

// drops the pages of the bytes [off..off+len-1] (len = 0: up to the end) of the file from the page cache
inline void drop_cached(int fd, uint64_t off, uint64_t len)
{
    posix_fadvise(fd, off_t(off), off_t(len), POSIX_FADV_DONTNEED);
}

// advises the kernel that the pages of the file are not accessed again (POSIX_FADV_NOREUSE)
inline void advise_no_reuse(int fd) { posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE); }

// starts the writeback of the bytes [off..off+len-1] (len = 0: up to the end) of the file without waiting for it
inline void start_writeback(int fd, uint64_t off, uint64_t len)
{
    sync_file_range(fd, off_t(off), off_t(len), SYNC_FILE_RANGE_WRITE);
}

// writes the bytes [off..off+len-1] of the file back to the disk and waits until they are written
inline void write_back(int fd, uint64_t off, uint64_t len)
{
    sync_file_range(fd, off_t(off), off_t(len),
        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
}
#endif

// ############################# MEMORY #############################

#ifdef _WIN32
// allocates bytes bytes aligned to alignment (a power of two); returns nullptr on failure; freed with free_aligned
inline void* allocate_aligned(uint64_t bytes, uint64_t alignment)
{
    return _aligned_malloc(size_t(bytes), size_t(alignment));
}

// same as allocate_aligned (malloc_count counts both)
inline void* allocate_aligned_counted(uint64_t bytes, uint64_t alignment) { return allocate_aligned(bytes, alignment); }

// frees memory allocated with allocate_aligned or allocate_aligned_counted
inline void free_aligned(void* data) { _aligned_free(data); }

// true <=> memory that is already allocated can be advised to use transparent huge pages (advise_huge_pages)
inline constexpr bool has_transparent_huge_pages = false;

// returns the size of a page in bytes
uint64_t page_size();

// returns the number of bytes of physical memory available to new allocations (0 if it is unknown)
uint64_t available_memory();

// advises the kernel to use (use = true) or not to use huge pages for a range of whole pages
inline void advise_huge_pages(void*, uint64_t, bool) {}

/**
 * @brief enables the privilege SeLockMemoryPrivilege of the process, which allocate_pages needs for large pages; the
 *        privilege has to be assigned to the user (local security policy "Lock pages in memory")
 * @return whether the privilege is enabled (the result of the first call is kept)
 */
bool enable_large_pages();

// returns the size of a large page in bytes (0 if the processor has none)
uint64_t large_page_bytes();

/**
 * @brief allocates committed pages; with large, they are large pages if enable_large_pages() succeeded and the
 *        system has enough contiguous physical memory, else pages of page_size() bytes
 * @param bytes number of bytes (rounded up to whole pages)
 * @param large true <=> large pages are tried first
 * @param is_large is set to true <=> the pages are large pages
 * @return the pages (zero-initialized), or nullptr on failure; freed with free_pages
 */
void* allocate_pages(uint64_t bytes, bool large, bool& is_large);

// frees pages allocated with allocate_pages
void free_pages(void* data);
#else
// allocates bytes bytes aligned to alignment (a power of two) with posix_memalign (malloc_count counts it); returns
// nullptr on failure; freed with free_aligned
inline void* allocate_aligned(uint64_t bytes, uint64_t alignment)
{
    void* data = nullptr;
    return posix_memalign(&data, size_t(alignment), size_t(bytes)) == 0 ? data : nullptr;
}

// allocates bytes bytes (a multiple of alignment) aligned to alignment (a power of two) with aligned_alloc, which
// malloc_count counts; returns nullptr on failure; freed with free_aligned
inline void* allocate_aligned_counted(uint64_t bytes, uint64_t alignment)
{
    return std::aligned_alloc(size_t(alignment), size_t(bytes));
}

// frees memory allocated with allocate_aligned or allocate_aligned_counted
inline void free_aligned(void* data) { std::free(data); }

// true <=> memory that is already allocated can be advised to use transparent huge pages (advise_huge_pages)
inline constexpr bool has_transparent_huge_pages = true;

// returns the size of a page in bytes
inline uint64_t page_size() { return uint64_t(sysconf(_SC_PAGESIZE)); }

// returns the number of bytes of physical memory available to new allocations, MemAvailable in /proc/meminfo (0 if it
// is unknown)
inline uint64_t available_memory()
{
    std::ifstream meminfo("/proc/meminfo");

    // a line "<name>: <value> kB"
    for (std::string line; std::getline(meminfo, line);) {
        std::istringstream fields(line);
        std::string name;
        uint64_t kib = 0;
        if (fields >> name >> kib && name == "MemAvailable:") return kib * 1024;
    }

    return 0;
}

// advises the kernel to use (use = true) or not to use transparent huge pages for a range of whole pages (madvise)
inline void advise_huge_pages(void* data, uint64_t bytes, bool use)
{
    madvise(data, size_t(bytes), use ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
}
#endif

// ############################# CPUS #############################

#ifdef _WIN32
/**
 * @brief a set of CPUs; CPU 64 * g + b is the CPU b of the processor group g (a group has at most 64 CPUs, one bit of
 *        its affinity mask each)
 */
class cpu_mask {
public:
    static constexpr uint64_t cpus_per_group = 8 * sizeof(uint64_t); // number of bits of an affinity mask

private:
    std::vector<uint64_t> groups; // [0..g-1] groups[i] is the affinity mask of the CPUs of group i in the set

public:
    // adds the CPU cpu to the set
    void set(uint64_t cpu)
    {
        if (cpu / cpus_per_group >= groups.size()) groups.resize(cpu / cpus_per_group + 1, 0);
        groups[cpu / cpus_per_group] |= uint64_t(1) << (cpu % cpus_per_group);
    }

    // returns whether the CPU cpu is in the set
    bool test(uint64_t cpu) const
    {
        return cpu / cpus_per_group < groups.size() && ((groups[cpu / cpus_per_group] >> (cpu % cpus_per_group)) & 1);
    }

    // returns the number of CPUs in the set
    uint64_t count() const
    {
        uint64_t num = 0;
        for (uint64_t mask : groups) num += uint64_t(__builtin_popcountll(mask));
        return num;
    }

    // returns a bound on the CPUs in the set: every CPU in it is smaller
    uint64_t end() const { return groups.size() * cpus_per_group; }

    // adds the CPUs of another set
    cpu_mask& operator|=(const cpu_mask& other)
    {
        if (other.groups.size() > groups.size()) groups.resize(other.groups.size(), 0);
        for (uint64_t g = 0; g < other.groups.size(); g++) groups[g] |= other.groups[g];
        return *this;
    }

    // returns whether both sets contain the same CPUs
    bool operator==(const cpu_mask& other) const
    {
        for (uint64_t g = 0; g < std::max(groups.size(), other.groups.size()); g++)
            if (group_mask(g) != other.group_mask(g)) return false;

        return true;
    }

    // returns the number of groups (the groups [num_groups()..] have no CPU in the set)
    uint64_t num_groups() const { return groups.size(); }

    // returns the affinity mask of the CPUs of a group in the set
    uint64_t group_mask(uint64_t g) const { return g < groups.size() ? groups[g] : 0; }

    // replaces the CPUs of a group in the set by the CPUs of an affinity mask
    void set_group_mask(uint64_t g, uint64_t mask)
    {
        if (g >= groups.size()) groups.resize(g + 1, 0);
        groups[g] = mask;
    }
};

// returns the number of online CPUs (of all processor groups), or 0 if it cannot be determined
uint64_t num_online_cpus();

// returns the set of all num_cpus = num_online_cpus() online CPUs
cpu_mask all_cpus(uint64_t num_cpus);

// returns the CPUs the process may run on: all online CPUs (a process can use the CPUs of all processor groups)
inline cpu_mask process_cpus() { return all_cpus(num_online_cpus()); }

// stores the CPUs the calling thread may run on in cpus; returns whether this succeeded
bool get_thread_cpus(cpu_mask& cpus);

/**
 * @brief restricts the calling thread to a set of CPUs; a set of CPUs of more than one processor group is set with
 *        SetThreadSelectedCpuSetMasks (Windows 11 and Windows Server 2022)
 * @return whether this succeeded
 */
bool set_thread_cpus(const cpu_mask& cpus);

// returns a string that is equal for all CPUs of the same physical core as CPU cpu, and only for them
std::string core_of_cpu(uint64_t cpu);
#else
// a set of CPUs (cpu_set_t)
class cpu_mask {
    cpu_set_t cpus; // the set

public:
    cpu_mask() { CPU_ZERO(&cpus); }

    // adds the CPU cpu to the set
    void set(uint64_t cpu) { CPU_SET(size_t(cpu), &cpus); }

    // returns whether the CPU cpu is in the set
    bool test(uint64_t cpu) const { return CPU_ISSET(size_t(cpu), &cpus); }

    // returns the number of CPUs in the set
    uint64_t count() const { return uint64_t(CPU_COUNT(&cpus)); }

    // returns a bound on the CPUs in the set: every CPU in it is smaller
    uint64_t end() const { return CPU_SETSIZE; }

    // adds the CPUs of another set
    cpu_mask& operator|=(const cpu_mask& other)
    {
        CPU_OR(&cpus, &cpus, &other.cpus);
        return *this;
    }

    // returns whether both sets contain the same CPUs
    bool operator==(const cpu_mask& other) const { return CPU_EQUAL(&cpus, &other.cpus); }

    // returns the cpu_set_t of the set
    cpu_set_t* native() { return &cpus; }
    const cpu_set_t* native() const { return &cpus; }
};

// returns the number of online CPUs, or 0 if it cannot be determined
inline uint64_t num_online_cpus()
{
    const auto num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return num_cpus > 0 ? uint64_t(num_cpus) : 0;
}

// returns the set of all num_cpus = num_online_cpus() online CPUs (the CPUs [0..num_cpus-1])
inline cpu_mask all_cpus(uint64_t num_cpus)
{
    cpu_mask cpus;
    for (uint64_t cpu = 0; cpu < num_cpus && cpu < cpus.end(); cpu++) cpus.set(cpu);
    return cpus;
}

// stores the CPUs the calling thread may run on in cpus; returns whether this succeeded
inline bool get_thread_cpus(cpu_mask& cpus) { return sched_getaffinity(0, sizeof(cpu_set_t), cpus.native()) == 0; }

// returns the CPUs the main thread of the process may run on (sched_getaffinity), or all online CPUs if they cannot be
// read
inline cpu_mask process_cpus()
{
    cpu_mask cpus;
    if (sched_getaffinity(getpid(), sizeof(cpu_set_t), cpus.native()) == 0 && cpus.count() > 0) return cpus;
    return all_cpus(num_online_cpus());
}

// restricts the calling thread to a set of CPUs; returns whether this succeeded
inline bool set_thread_cpus(const cpu_mask& cpus)
{
    return sched_setaffinity(0, sizeof(cpu_set_t), cpus.native()) == 0;
}

// returns a string that is equal for all CPUs of the same physical core as CPU cpu (thread_siblings_list), and only
// for them
inline std::string core_of_cpu(uint64_t cpu)
{
    std::ifstream siblings_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
    std::string siblings;
    if (!(siblings_file >> siblings)) siblings = "cpu " + std::to_string(cpu);
    return siblings;
}
#endif

// ############################# CONSOLE, ENVIRONMENT, TEMPORARY FILES #############################

#ifdef _WIN32
// returns whether a stream is a terminal
inline bool is_terminal(FILE* stream) { return _isatty(_fileno(stream)) != 0; }

// sets an environment variable
inline void set_env(const char* name, const std::string& value) { _putenv_s(name, value.c_str()); }

// removes an environment variable
inline void unset_env(const char* name) { _putenv_s(name, ""); }

// returns whether a path lies on a RAM disk (false if the drive type cannot be determined)
bool is_ram_disk(const std::string& path);

// the directory for temporary files if the system does not name one: the current directory
inline constexpr const char* fallback_temporary_directory = ".";

/**
 * @brief prepares the process: standard output and standard error in binary mode (no "\r\n" for "\n", as on Linux),
 *        escape sequences in the console, a stack of STRING_UTILS_STACK_BYTES bytes for the threads of OpenMP (the main
 *        thread gets it from the linker) and large pages (see enable_large_pages)
 */
void init();

// true after init() has run; init() runs before main in every program that includes this header
inline const bool is_initialized = (init(), true);
#else
// returns whether a stream is a terminal
inline bool is_terminal(FILE* stream) { return isatty(fileno(stream)) != 0; }

// sets an environment variable
inline void set_env(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }

// removes an environment variable
inline void unset_env(const char* name) { unsetenv(name); }

// returns whether a path lies on a RAM disk, a tmpfs or ramfs file system (false if it cannot be determined)
inline bool is_ram_disk(const std::string& path)
{
    struct statfs info;
    if (statfs(path.c_str(), &info) != 0) return false;
    return info.f_type == TMPFS_MAGIC || info.f_type == RAMFS_MAGIC;
}

// the directory for temporary files if the system does not name one
inline constexpr const char* fallback_temporary_directory = "/tmp";
#endif

} // namespace platform

#endif // STRING_UTILS_MISC_PLATFORM_HPP
