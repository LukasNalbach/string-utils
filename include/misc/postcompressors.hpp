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
 * @file postcompressors.hpp
 * @brief general-purpose compressors (external programs) that a compressor (see compressor_cli.hpp) compresses its
 *        output file with (option -pc) and decompresses it with
 */

#ifndef STRING_UTILS_MISC_POSTCOMPRESSORS_HPP
#define STRING_UTILS_MISC_POSTCOMPRESSORS_HPP

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#include <misc/files.hpp>
#include <misc/log.hpp>

// the postcompressors (none: the output file is not compressed)
enum class postcompressor_kind {
    none, bsc, zstd, xz, lzma, gzip, pigz, bzip2, pbzip2, lbzip2, brotli, lz4, lzop, lzip, plzip, bzip3, sevenzip
};

// a postcompressor
struct postcompressor_spec {
    postcompressor_kind kind;
    const char* name;       // name of the compressor (option -pc) and extension of the files written with it
    const char* binary;     // name of the program to run (has to be on the PATH)
    uint32_t min_level;     // minimum value of -<level> (for bsc and bzip3 the block size in MB)
    uint32_t max_level;     // maximum value of -<level>
    uint32_t default_level; // default value of -<level>; for bsc, 0 <=> chosen by bsc_block_bytes
};

// the supported postcompressors, each as { kind, name, binary, min_level, max_level, default_level }
inline constexpr std::array<postcompressor_spec, 17> postcompressors { {
    { postcompressor_kind::bsc, "bsc", "bsc", 1, 2047, 0 },
    { postcompressor_kind::zstd, "zstd", "zstd", 1, 22, 4 },
    { postcompressor_kind::xz, "xz", "xz", 0, 9, 6 },
    { postcompressor_kind::lzma, "lzma", "xz", 0, 9, 6 },
    { postcompressor_kind::gzip, "gzip", "gzip", 1, 9, 6 },
    { postcompressor_kind::pigz, "pigz", "pigz", 1, 9, 6 },
    { postcompressor_kind::bzip2, "bzip2", "bzip2", 1, 9, 9 },
    { postcompressor_kind::pbzip2, "pbzip2", "pbzip2", 1, 9, 9 },
    { postcompressor_kind::lbzip2, "lbzip2", "lbzip2", 1, 9, 9 },
    { postcompressor_kind::brotli, "brotli", "brotli", 0, 11, 11 },
    { postcompressor_kind::lz4, "lz4", "lz4", 1, 12, 1 },
    { postcompressor_kind::lzop, "lzop", "lzop", 1, 9, 3 },
    { postcompressor_kind::lzip, "lzip", "lzip", 0, 9, 6 },
    { postcompressor_kind::plzip, "plzip", "plzip", 0, 9, 6 },
    { postcompressor_kind::bzip3, "bzip3", "bzip3", 1, 511, 16 },
    { postcompressor_kind::sevenzip, "7z", "7z", 0, 9, 5 },
    { postcompressor_kind::none, "none", "", 0, 0, 0 }
} };

/**
 * @brief returns the compressor with a given name
 * @param name name of a compressor
 * @return the compressor, or nullptr if there is none with that name
 */
inline const postcompressor_spec* find_postcompressor(const std::string& name)
{
    for (const postcompressor_spec& spec : postcompressors)
        if (name == spec.name) return &spec;

    return nullptr;
}

/**
 * @brief returns whether the program of a compressor is on the PATH
 * @param spec a compressor
 */
inline bool postcompressor_installed(const postcompressor_spec& spec)
{
    if (spec.kind == postcompressor_kind::none) return true;
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
#ifdef _WIN32
    const char separator = ';';
    const std::string suffix = ".exe";
#else
    const char separator = ':';
    const std::string suffix;
#endif
    const std::string dirs(path);
    const std::string binary(spec.binary);

    for (size_t begin = 0; begin <= dirs.size();) {
        size_t end = dirs.find(separator, begin);
        if (end == std::string::npos) end = dirs.size();
        const std::filesystem::path dir = dirs.substr(begin, end - begin);
        std::error_code error;
        if (!dir.empty() && std::filesystem::is_regular_file(dir / binary, error)) return true;
        if (!dir.empty() && !suffix.empty() && std::filesystem::is_regular_file(dir / (binary + suffix), error))
            return true;
        begin = end + 1;
    }

    return false;
}

// returns a path quoted as one argument of a shell command
inline std::string shell_quoted(const std::string& path)
{
#ifdef _WIN32
    return "\"" + path + "\"";
#else
    std::string quoted = "'";

    for (char c : path) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }

    return quoted + "'";
#endif
}

// estimated memory usage of bsc in bytes: bsc_fixed_bytes + bsc_bytes_per_block_byte * block size
inline constexpr uint64_t bsc_fixed_bytes = uint64_t(20) << 20;
inline constexpr double bsc_bytes_per_block_byte = 5.5;
// minimum and maximum block size in bytes (bsc interprets -b<size> as bytes if size >= 10000, else as MB)
inline constexpr uint64_t bsc_min_block = 10000;
inline constexpr uint64_t bsc_max_block = uint64_t(2047) << 20;

/**
 * @brief returns the block size of bsc if none is given with -<level>: the largest block size for which the
 *        estimated memory usage of bsc is at most peak
 * @param peak peak memory usage of the construction in bytes (measured with malloc_count)
 * @return the block size in bytes
 */
inline uint64_t bsc_block_bytes(uint64_t peak)
{
#if STRING_UTILS_USE_MALLOC_COUNT
    const uint64_t available = peak > bsc_fixed_bytes ? peak - bsc_fixed_bytes : 0;
    return std::clamp<uint64_t>(uint64_t(double(available) / bsc_bytes_per_block_byte), bsc_min_block, bsc_max_block);
#else
    // without malloc_count, the peak is not measured
    (void) peak;
    return bsc_max_block;
#endif
}

// returns the block size of bzip3 in MB: the size of the input (bytes) rounded up to MB, at most level
inline uint64_t bzip3_block_mb(uint64_t bytes, uint64_t level)
{
    return std::clamp<uint64_t>((bytes + (uint64_t(1) << 20) - 1) >> 20, 1, level);
}

// returns the number of threads of bzip3 for a file with the given number of blocks
inline uint64_t bzip3_threads(uint64_t blocks, uint64_t threads) { return std::clamp<uint64_t>(blocks, 1, threads); }

/**
 * @brief returns the number of blocks of a file written by bzip3; the file consists of
 *        1. "BZ3v1" and the block size (4 bytes),
 *        2. per block: its compressed length (4 bytes), its length (4 bytes) and its compressed bytes
 * @param path path of the file
 * @return the number of blocks, or 0 if the file does not have this format
 */
inline uint64_t bzip3_blocks(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    char magic[5];
    in.read(magic, 5);
    if (!in || std::string(magic, 5) != "BZ3v1") return 0;
    in.seekg(4, std::ios::cur);
    uint64_t blocks = 0;

    while (true) {
        unsigned char lengths[8];
        in.read((char*) lengths, 8);
        if (in.gcount() == 0) return blocks;
        if (in.gcount() != 8) return 0;
        const int32_t coded = int32_t(uint32_t(lengths[0]) | uint32_t(lengths[1]) << 8 | uint32_t(lengths[2]) << 16
            | uint32_t(lengths[3]) << 24);
        if (coded < 0 || !in.seekg(coded, std::ios::cur)) return 0;
        blocks++;
    }
}

// source of the progress of a program: the number of bytes written to its standard input (input_bytes), or the
// percentages it prints to its standard output or to its error output
enum class progress_source { input_bytes, tool_stdout, tool_stderr };

// a command line and the source of its progress
struct postcompressor_command {
    std::string command;
    progress_source progress = progress_source::input_bytes;
};

/**
 * @brief returns the command that compresses a file
 * @param spec the compressor
 * @param from path of the input file; a program that reads its standard input does not open it (see
 *        run_postcompressor)
 * @param from_bytes size of the input file in bytes
 * @param to path of the output file
 * @param threads number of threads to use
 * @param level value of -<level>; for bsc the block size in bytes (see bsc_block_bytes)
 */
inline postcompressor_command compress_command(const postcompressor_spec& spec, const std::string& from,
    uint64_t from_bytes, const std::string& to, uint64_t threads, uint64_t level)
{
    const std::string p = std::to_string(threads);
    const std::string q = std::to_string(level);
    const std::string in = shell_quoted(from);
    const std::string out = shell_quoted(to);
    const std::string ultra = level > 19 ? " --ultra" : "";
    using enum postcompressor_kind;

    switch (spec.kind) {
        case zstd: return { "zstd -c -q -" + q + ultra + " -T" + p + " > " + out };
        case xz: return { "xz -c -q -" + q + " -T" + p + " > " + out };
        case lzma: return { "xz --format=lzma -c -q -" + q + " > " + out };
        case gzip: return { "gzip -c -q -" + q + " > " + out };
        case pigz: return { "pigz -c -q -" + q + " -p " + p + " > " + out };
        case bzip2: return { "bzip2 -c -q -" + q + " > " + out };
        case pbzip2: return { "pbzip2 -c -q -" + q + " -p" + p + " > " + out };
        case lbzip2: return { "lbzip2 -c -q -" + q + " -n " + p + " > " + out };
        case brotli: return { "brotli -c -q " + q + " > " + out };
        case lz4: return { "lz4 -c -q -" + q + " > " + out };
        case lzop: return { "lzop -c -q -" + q + " > " + out };
        case lzip: return { "lzip -c -q -" + q + " > " + out };
        case plzip: return { "plzip -c -q -" + q + " -n " + p + " > " + out };
        case bzip3: {
            const uint64_t block = bzip3_block_mb(from_bytes, level);
            const uint64_t blocks = std::max<uint64_t>(1, (from_bytes + (block << 20) - 1) / (block << 20));
            return { "bzip3 -e -c -j " + std::to_string(bzip3_threads(blocks, threads)) + " -b "
                + std::to_string(block) + " > " + out };
        }
        case sevenzip: return { "7z a -t7z -mx=" + q + " -mmt=" + p + " -bso0 -bsp2 " + out + " " + in + " 2>&1",
            progress_source::tool_stderr };
        // -e2: adaptive coder, -t: the blocks are compressed one after another (each with all threads),
        // -p: without the LZP stage; bsc takes the number of threads from OpenMP
        case bsc:
#ifdef _WIN32
            return { "bsc e " + in + " " + out + " -b" + q + " -e2tp", progress_source::tool_stdout };
#else
            return { "OMP_NUM_THREADS=" + p + " bsc e " + in + " " + out + " -b" + q + " -e2tp",
                progress_source::tool_stdout };
#endif
        case none: break;
    }

    return {};
}

/**
 * @brief returns the command that decompresses a file
 * @param spec the compressor the file was compressed with
 * @param from path of the input file; a program that reads its standard input does not open it (see
 *        run_postcompressor)
 * @param to path of the output file
 * @param threads number of threads to use
 */
inline postcompressor_command decompress_command(const postcompressor_spec& spec, const std::string& from,
    const std::string& to, uint64_t threads)
{
    const std::string p = std::to_string(threads);
    const std::string in = shell_quoted(from);
    const std::string out = shell_quoted(to);
    using enum postcompressor_kind;

    switch (spec.kind) {
        case zstd: return { "zstd -d -c -q > " + out };
        case xz: return { "xz -d -c -q -T" + p + " > " + out };
        case lzma: return { "xz --format=lzma -d -c -q > " + out };
        case gzip: return { "gzip -d -c -q > " + out };
        case pigz: return { "pigz -d -c -q -p " + p + " > " + out };
        case bzip2: return { "bzip2 -d -c -q > " + out };
        case pbzip2: return { "pbzip2 -d -c -q -p" + p + " > " + out };
        case lbzip2: return { "lbzip2 -d -c -q -n " + p + " > " + out };
        case brotli: return { "brotli -d -c > " + out };
        case lz4: return { "lz4 -d -c -q > " + out };
        case lzop: return { "lzop -d -c -q > " + out };
        case lzip: return { "lzip -d -c -q > " + out };
        case plzip: return { "plzip -d -c -q -n " + p + " > " + out };
        case bzip3: return { "bzip3 -d -c -j " + std::to_string(bzip3_threads(bzip3_blocks(from), threads)) + " > "
            + out };
        // 2>&1 precedes the redirection of stdout: the progress (stderr) is written to the pipe, the output to out
        case sevenzip: return { "7z x -so -bsp2 -bso0 " + in + " 2>&1 > " + out, progress_source::tool_stderr };
#ifdef _WIN32
        case bsc: return { "bsc d " + in + " " + out, progress_source::tool_stdout };
#else
        case bsc: return { "OMP_NUM_THREADS=" + p + " bsc d " + in + " " + out, progress_source::tool_stdout };
#endif
        case none: break;
    }

    return {};
}

/**
 * @brief returns the maximum resident set size in bytes of the terminated child processes of this process (0 if
 *        the platform does not report it)
 */
inline uint64_t children_peak_bytes()
{
#ifdef _WIN32
    return 0;
#else
    rusage usage {};
    if (getrusage(RUSAGE_CHILDREN, &usage) != 0) return 0;
#ifdef __APPLE__
    return uint64_t(usage.ru_maxrss);
#else
    return uint64_t(usage.ru_maxrss) * 1024; // ru_maxrss is in kilobytes on Linux
#endif
#endif
}

// option with which a compressor executes itself to measure the peak memory usage of a postcompressor (see
// run_and_write_peak and run_postcompressor)
inline constexpr const char* measure_peak_option = "--measure-peak";

/**
 * @brief runs a command and writes the peak memory usage in bytes of its processes to a file (see measure_peak_option)
 * @param argc number of arguments (4)
 * @param argv the arguments: the program name, measure_peak_option, the path of the file to write the peak to and the
 *        command
 * @return the exit status of the command
 */
inline int run_and_write_peak(int argc, char** argv)
{
#ifdef _WIN32
    (void) argc;
    (void) argv;
    return 1;
#else
    if (argc != 4) return 2;
    const int status = std::system(argv[3]);
    std::ofstream(argv[2]) << children_peak_bytes() << std::endl;
    return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/**
 * @brief runs the command of a compressor. If the program reads its standard input, the file feed_path is
 *        written to it through a pipe in blocks of 1 MiB and the progress is the number of bytes written; else
 *        (bsc and 7z), the progress is read from the percentages the program prints. The error output of the
 *        program is written to the file err_path.
 * @param command the command (see compress_command and decompress_command)
 * @param feed_path path of the file that is written to the standard input of the program
 * @param err_path path of the file the error output of the program is written to
 * @param log whether the progress is printed in the line of the running phase (see phase_progress)
 * @param peak_path path of the file used to measure the peak memory usage of the program (see measure_peak_option);
 *        empty <=> it is not measured
 * @param peak is set to the peak memory usage of the program in bytes, if it is measured
 * @return whether the command succeeded
 */
inline bool run_postcompressor(const postcompressor_command& command, const std::string& feed_path,
    const std::string& err_path, bool log, const std::string& peak_path = "", uint64_t* peak = nullptr)
{
    std::string line = command.command;
    if (command.progress != progress_source::tool_stderr) line += " 2> " + shell_quoted(err_path);
#ifdef __linux__
    // path of this executable (in the command, /proc/self/exe is the path of the shell)
    std::error_code self_error;
    const std::string self = peak_path.empty() ? std::string()
        : std::filesystem::read_symlink("/proc/self/exe", self_error).string();
    const bool measured = !self.empty() && !self_error;
    if (measured) {
        line = shell_quoted(self) + " " + measure_peak_option + " " + shell_quoted(peak_path) + " "
            + shell_quoted(line);
    }
#else
    const bool measured = false;
#endif
    const bool feed = command.progress == progress_source::input_bytes;
    int status = -1;
#ifdef _WIN32
    FILE* pipe = _popen(line.c_str(), feed ? "wb" : "rb");
#else
    FILE* pipe = popen(line.c_str(), feed ? "w" : "r");
#endif
    if (pipe == nullptr) return false;

    if (feed) {
#ifndef _WIN32
        // ignore SIGPIPE (raised if the program stops reading its input)
        auto previous = std::signal(SIGPIPE, SIG_IGN);
#endif
        static constexpr uint64_t feed_block = uint64_t(1) << 20;
        std::error_code error;
        const uint64_t total = std::filesystem::file_size(feed_path, error);
        phase_progress progress(log && !error, total);
        direct_ifstream in(feed_path);
        std::string buffer;
        resize_no_init(buffer, feed_block);
        uint64_t done = 0;

        while (!error && done < total && in.good()) {
            const uint64_t len = std::min<uint64_t>(feed_block, total - done);
            in.read(buffer.data(), (std::streamsize) len);
            if ((uint64_t) in.gcount() != len || std::fwrite(buffer.data(), 1, len, pipe) != len) break;
            done += len;
            progress.reached(done);
        }

        progress.finish();
#ifdef _WIN32
        status = _pclose(pipe);
#else
        status = pclose(pipe);
        std::signal(SIGPIPE, previous);
#endif
        if (error || done != total) return false;
    } else {
        phase_progress progress(log, 100);
        uint64_t number = 0;
        bool digits = false;

        for (int c = std::fgetc(pipe); c != EOF; c = std::fgetc(pipe)) {
            if (c >= '0' && c <= '9') {
                number = number * 10 + uint64_t(c - '0');
                digits = true;
            } else {
                if (c == '%' && digits && number <= 100) progress.reached(number);
                number = 0;
                digits = false;
            }
        }

        progress.finish();
#ifdef _WIN32
        status = _pclose(pipe);
#else
        status = pclose(pipe);
#endif
    }

    if (measured && peak != nullptr) {
        std::ifstream peak_file(peak_path);
        uint64_t bytes = 0;
        if (peak_file >> bytes) *peak = bytes;
    }

    return status == 0;
}

/**
 * @brief returns the error output of a program (without trailing line breaks)
 * @param err_path path of the file the error output was written to (see run_postcompressor)
 */
inline std::string postcompressor_errors(const std::string& err_path)
{
    std::ifstream err(err_path);
    std::stringstream text;
    text << err.rdbuf();
    std::string errors = text.str();
    while (!errors.empty() && (errors.back() == '\n' || errors.back() == '\r')) errors.pop_back();
    return errors;
}

#endif // STRING_UTILS_MISC_POSTCOMPRESSORS_HPP
