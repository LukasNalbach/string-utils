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

// the Windows functions of misc/platform.hpp that need <windows.h> (compiled only on Windows, see CMakeLists.txt)

// Windows only (the build adds this file only on Windows)
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <fcntl.h>
#include <io.h>
#include <omp.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include <misc/platform.hpp>

namespace {

// largest number of bytes one ReadFile or WriteFile transfers: the largest power of two a DWORD holds (a multiple
// of every sector size)
constexpr uint64_t max_transfer_bytes = std::bit_floor(uint64_t(std::numeric_limits<DWORD>::max()));

// number of bits of a DWORD, the low part of a 64-bit offset in an OVERLAPPED
constexpr uint64_t dword_bits = 8 * sizeof(DWORD);

// returns the handle of a descriptor
HANDLE handle_of(int fd) { return reinterpret_cast<HANDLE>(_get_osfhandle(fd)); }

// converts a UTF-8 string to UTF-16
std::wstring to_wide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

// converts a UTF-16 string to UTF-8
std::string to_utf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size_t(std::max(length, 0)), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), text.data(), length, nullptr, nullptr);
    return text;
}

// returns the event of the calling thread on which its positioned reads and writes wait (manual reset)
HANDLE thread_event()
{
    struct event_t {
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~event_t() { if (handle != nullptr) CloseHandle(handle); }
    };

    thread_local event_t event;
    return event.handle;
}

// returns an OVERLAPPED for an operation at the offset off that signals the event of the calling thread
OVERLAPPED overlapped_at(uint64_t off)
{
    OVERLAPPED overlapped {};
    overlapped.Offset = DWORD(off);
    overlapped.OffsetHigh = DWORD(off >> dword_bits);
    overlapped.hEvent = thread_event();
    return overlapped;
}

/**
 * @brief waits for an operation on an overlapped handle that has been started
 * @param handle the handle
 * @param overlapped the OVERLAPPED of the operation
 * @param started the result of the call that started the operation
 * @return the number of bytes transferred, 0 at the end of the file, or -1 on an error
 */
int64_t finish_operation(HANDLE handle, OVERLAPPED& overlapped, BOOL started)
{
    if (!started) {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF) return 0;
        if (error != ERROR_IO_PENDING) return -1;
    }

    DWORD num_bytes = 0;
    if (GetOverlappedResult(handle, &overlapped, &num_bytes, TRUE)) return int64_t(num_bytes);
    return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
}

/**
 * @brief opens a file with FILE_FLAG_OVERLAPPED
 * @param path path of the file (UTF-8)
 * @param access how the file is opened
 * @param share the access other handles of the file may have (FILE_SHARE_*)
 * @param flags further flags (FILE_FLAG_*)
 * @return the handle, or INVALID_HANDLE_VALUE
 */
HANDLE create_file(const std::string& path, platform::file_access access, DWORD share, DWORD flags)
{
    DWORD rights = FILE_READ_ATTRIBUTES;
    DWORD creation = OPEN_EXISTING;

    switch (access) {
        case platform::file_access::read: rights |= GENERIC_READ; break;
        case platform::file_access::write: rights |= GENERIC_WRITE; creation = CREATE_ALWAYS; break;
        case platform::file_access::append: rights |= GENERIC_WRITE; creation = OPEN_ALWAYS; break;
        case platform::file_access::read_write: rights |= GENERIC_READ | GENERIC_WRITE; creation = CREATE_ALWAYS; break;
    }

    return CreateFileW(to_wide(path).c_str(), rights, share, nullptr, creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | flags, nullptr);
}

// returns a descriptor of the C runtime for a handle; the handle is closed if this fails (then -1 is returned)
int to_descriptor(HANDLE handle, platform::file_access access)
{
    if (handle == INVALID_HANDLE_VALUE) return -1;
    int flags = _O_BINARY;

    switch (access) {
        case platform::file_access::read: flags |= _O_RDONLY; break;
        case platform::file_access::write: flags |= _O_WRONLY; break;
        case platform::file_access::append: flags |= _O_WRONLY | _O_APPEND; break;
        case platform::file_access::read_write: flags |= _O_RDWR; break;
    }

    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), flags);
    if (fd < 0) CloseHandle(handle);
    return fd;
}

// returns whether unbuffered I/O on the volume of a file works with buffers, offsets and lengths aligned to
// direct_io_alignment: the logical sector size divides it, and so does the physical one used for performance
bool supports_direct_io_alignment(HANDLE handle)
{
    FILE_STORAGE_INFO info {};
    if (!GetFileInformationByHandleEx(handle, FileStorageInfo, &info, sizeof(info))) return false;
    const uint64_t logical = info.LogicalBytesPerSector;
    const uint64_t physical = info.PhysicalBytesPerSectorForPerformance;
    return logical != 0 && platform::direct_io_alignment % logical == 0
        && (physical == 0 || platform::direct_io_alignment % physical == 0);
}

// calls fnc(info) for each record of GetLogicalProcessorInformationEx(relation)
void for_each_processor_record(LOGICAL_PROCESSOR_RELATIONSHIP relation,
    const std::function<void(const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX&)>& fnc)
{
    DWORD length = 0;
    GetLogicalProcessorInformationEx(relation, nullptr, &length);
    if (length == 0) return;
    std::vector<uint8_t> buffer(length);
    auto* records = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(relation, records, &length)) return;

    for (DWORD pos = 0; pos < length;) {
        const auto* record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + pos);
        fnc(*record);
        pos += record->Size;
    }
}

// SetThreadSelectedCpuSetMasks of kernel32.dll (Windows 11 and Windows Server 2022), nullptr if it does not exist
using set_selected_cpu_set_masks_t = BOOL (WINAPI*)(HANDLE, PGROUP_AFFINITY, USHORT);

set_selected_cpu_set_masks_t set_selected_cpu_set_masks()
{
    static const auto function = reinterpret_cast<set_selected_cpu_set_masks_t>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadSelectedCpuSetMasks")));
    return function;
}

// true <=> the calling thread has selected CPU sets of more than one processor group (see set_thread_cpus)
thread_local bool has_selected_cpu_sets = false;

} // namespace

// ############################# FILES #############################

int platform::open_file(const std::string& path, file_access access, bool direct)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = create_file(path, access, share, direct ? FILE_FLAG_NO_BUFFERING : 0);

    if (direct && handle != INVALID_HANDLE_VALUE && !supports_direct_io_alignment(handle)) {
        CloseHandle(handle);
        return -1;
    }

    return to_descriptor(handle, access);
}

int platform::open_pinned(const std::string& path)
{
    // the file may be read, renamed and replaced meanwhile (as on Linux); reopen_path follows a rename
    return to_descriptor(create_file(path, file_access::read, FILE_SHARE_READ | FILE_SHARE_DELETE, 0), file_access::read);
}

std::string platform::reopen_path(int fd, const std::string& path)
{
    // the current path of the opened file
    const HANDLE handle = handle_of(fd);
    const DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return path;
    std::wstring wide(length, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, wide.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= length) return path;
    wide.resize(written);
    return to_utf8(wide);
}

int64_t platform::read_at(int fd, void* data, uint64_t size, uint64_t off)
{
    const HANDLE handle = handle_of(fd);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlapped = overlapped_at(off);
    const BOOL started = ReadFile(handle, data, DWORD(std::min(size, max_transfer_bytes)), nullptr, &overlapped);
    return finish_operation(handle, overlapped, started);
}

int64_t platform::write_at(int fd, const void* data, uint64_t size, uint64_t off)
{
    const HANDLE handle = handle_of(fd);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlapped = overlapped_at(off);
    const BOOL started = WriteFile(handle, data, DWORD(std::min(size, max_transfer_bytes)), nullptr, &overlapped);
    return finish_operation(handle, overlapped, started);
}

platform::file_offset_t platform::seek_end(int fd)
{
    LARGE_INTEGER size {};
    return GetFileSizeEx(handle_of(fd), &size) ? file_offset_t(size.QuadPart) : -1;
}

bool platform::set_size(int fd, uint64_t size)
{
    FILE_END_OF_FILE_INFO info {};
    info.EndOfFile.QuadPart = LONGLONG(size);
    return SetFileInformationByHandle(handle_of(fd), FileEndOfFileInfo, &info, sizeof(info)) != 0;
}

void platform::make_sparse(int fd)
{
    // file systems without sparse files (e.g., FAT) refuse it; the file is used as it is then
    const HANDLE handle = handle_of(fd);
    OVERLAPPED overlapped = overlapped_at(0);
    const BOOL started = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, nullptr, &overlapped);
    finish_operation(handle, overlapped, started);
}

// ############################# MEMORY #############################

uint64_t platform::page_size()
{
    SYSTEM_INFO info {};
    GetSystemInfo(&info);
    return info.dwPageSize;
}

uint64_t platform::available_memory()
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullAvailPhys) : 0;
}

bool platform::enable_large_pages()
{
    static const bool enabled = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
        TOKEN_PRIVILEGES privileges {};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        // AdjustTokenPrivileges succeeds without enabling a privilege the user does not have (ERROR_NOT_ALL_ASSIGNED)
        const bool is_enabled = LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &privileges.Privileges[0].Luid)
            && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr)
            && GetLastError() == ERROR_SUCCESS;
        CloseHandle(token);
        return is_enabled;
    }();

    return enabled;
}

uint64_t platform::large_page_bytes() { return GetLargePageMinimum(); }

void* platform::allocate_pages(uint64_t bytes, bool large, bool& is_large)
{
    is_large = false;
    const uint64_t large_bytes = large_page_bytes();

    if (large && large_bytes != 0 && enable_large_pages()) {
        const uint64_t rounded = (bytes + large_bytes - 1) / large_bytes * large_bytes;
        void* data = VirtualAlloc(nullptr, SIZE_T(rounded), MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);

        if (data != nullptr) {
            is_large = true;
            return data;
        }
    }

    return VirtualAlloc(nullptr, SIZE_T(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void platform::free_pages(void* data) { if (data != nullptr) VirtualFree(data, 0, MEM_RELEASE); }

// ############################# CPUS #############################

uint64_t platform::num_online_cpus() { return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }

platform::cpu_mask platform::all_cpus(uint64_t)
{
    cpu_mask cpus;

    for_each_processor_record(RelationGroup, [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& record) {
        for (WORD g = 0; g < record.Group.ActiveGroupCount; g++)
            cpus.set_group_mask(g, uint64_t(record.Group.GroupInfo[g].ActiveProcessorMask));
    });

    return cpus;
}

bool platform::get_thread_cpus(cpu_mask& cpus)
{
    GROUP_AFFINITY affinity {};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &affinity)) return false;
    cpus = cpu_mask();
    cpus.set_group_mask(affinity.Group, uint64_t(affinity.Mask));
    return true;
}

bool platform::set_thread_cpus(const cpu_mask& cpus)
{
    // the affinity masks of the groups with CPUs in the set
    std::vector<GROUP_AFFINITY> masks;

    for (uint64_t g = 0; g < cpus.num_groups(); g++) {
        if (cpus.group_mask(g) == 0) continue;
        GROUP_AFFINITY affinity {};
        affinity.Mask = KAFFINITY(cpus.group_mask(g));
        affinity.Group = WORD(g);
        masks.push_back(affinity);
    }

    if (masks.empty()) return false;
    const set_selected_cpu_set_masks_t select = set_selected_cpu_set_masks();

    // a thread runs in one processor group: a set within one group is its affinity
    if (masks.size() == 1) {
        if (!SetThreadGroupAffinity(GetCurrentThread(), &masks[0], nullptr)) return false;

        if (has_selected_cpu_sets && select != nullptr) {
            select(GetCurrentThread(), nullptr, 0);
            has_selected_cpu_sets = false;
        }

        return true;
    }

    // CPUs of several groups are selected as CPU sets (the thread may move between the groups)
    if (select == nullptr || !select(GetCurrentThread(), masks.data(), USHORT(masks.size()))) return false;
    has_selected_cpu_sets = true;
    return true;
}

std::string platform::core_of_cpu(uint64_t cpu)
{
    // [0..c-1] core_index[x] = the index of the core of CPU x, or c if it is not known
    static const std::vector<uint64_t> core_index = [] {
        std::vector<uint64_t> cores;
        uint64_t num_cores = 0;

        for_each_processor_record(RelationProcessorCore, [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& record) {
            for (WORD i = 0; i < record.Processor.GroupCount; i++) {
                const GROUP_AFFINITY& affinity = record.Processor.GroupMask[i];

                for (uint64_t bit = 0; bit < cpu_mask::cpus_per_group; bit++) {
                    if (((uint64_t(affinity.Mask) >> bit) & 1) == 0) continue;
                    const uint64_t x = affinity.Group * cpu_mask::cpus_per_group + bit;
                    if (x >= cores.size()) cores.resize(x + 1, std::numeric_limits<uint64_t>::max());
                    cores[x] = num_cores;
                }
            }

            num_cores++;
        });

        return cores;
    }();

    if (cpu < core_index.size() && core_index[cpu] != std::numeric_limits<uint64_t>::max())
        return "core " + std::to_string(core_index[cpu]);

    return "cpu " + std::to_string(cpu);
}

// ############################# CONSOLE, ENVIRONMENT, TEMPORARY FILES #############################

bool platform::is_ram_disk(const std::string& path)
{
    const std::wstring wide = to_wide(path);
    // the volume path is a prefix of the path, or a drive name
    std::vector<wchar_t> volume(std::max<size_t>(wide.size() + 1, MAX_PATH));
    if (!GetVolumePathNameW(wide.c_str(), volume.data(), DWORD(volume.size()))) return false;
    return GetDriveTypeW(volume.data()) == DRIVE_RAMDISK;
}

void platform::init()
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // the progress lines and the colors of the tests use escape sequences
    for (DWORD stream : { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE }) {
        const HANDLE handle = GetStdHandle(stream);
        DWORD mode = 0;
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode))
            SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

#if defined(STRING_UTILS_STACK_BYTES) && defined(KMP_VERSION_MAJOR)
    // the threads of LLVM's OpenMP runtime (libgomp gets the stack size of the executable)
    kmp_set_stacksize_s(size_t(STRING_UTILS_STACK_BYTES));
#endif

    enable_large_pages();
}

#endif // _WIN32
