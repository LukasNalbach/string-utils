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
 * @file compressor_cli.hpp
 * @brief the command-line interface of a compressor:
 *        "<tool> [options] <input_file>" compresses <input_file> into <base_name>.<ending>[.<postcompressor>],
 *        "<tool> -d [options] <input_file>" restores it; the common options, the help, the names of the output files,
 *        the measurement file, the post-compression and the decompression of a parsing into a file
 */

#ifndef STRING_UTILS_MISC_COMPRESSOR_CLI_HPP
#define STRING_UTILS_MISC_COMPRESSOR_CLI_HPP

#include <omp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <coding/byte_codes.hpp>
#include <fasta/restore.hpp>
#include <fasta/split.hpp>
#include <misc/cli_help.hpp>
#include <misc/extraction.hpp>
#include <misc/files.hpp>
#include <misc/log.hpp>
#include <misc/measurement.hpp>
#include <misc/postcompressors.hpp>
#include <misc/temp_files.hpp>

// the width of a line of the help of a compressor (the width of a terminal)
inline constexpr uint64_t compressor_help_width = 80;

// the first byte of a FASTA file (the start of its first header line)
inline constexpr char fasta_header_start = '>';

/** @brief what a compressor supports besides the common options */
struct compressor_spec {
    std::string_view name; // the name of the tool, e.g. "xyz-comp"
    std::string_view ending; // the ending of its files without the dot, e.g. "xyz"
    std::string_view description; // what the tool does, e.g. "compresses a text into its LZ77 parsing"
    std::string_view default_postcompressor; // the default of -pc; empty <=> no post-compression (-pc, -<quality>)
    bool fasta = false; // whether the tool has a FASTA mode (-fasta)
    bool ranges = false; // whether -d can write ranges of the text (-ranges)
};

/** @brief the common options of a compressor */
struct compressor_cli_t {
    bool decompress = false; // -d
    std::string path_input_file; // <input_file>
    // compression: <base_name>.<ending>[.<postcompressor>] (see -o); -d: the restored text
    std::string path_output_file;
    uint64_t threads = omp_get_max_threads(); // -t
    // -pc (with -d: from the ending of <input_file>); nullptr <=> the tool does not post-compress
    const postcompressor_spec* postcompressor = nullptr;
    uint64_t quality = 0; // -<quality> (or the default of the postcompressor)
    bool quality_given = false; // whether -<quality> was given
    bool fasta = false; // whether <input_file> is compressed in the FASTA mode (see -fasta)
    std::string path_ranges_file; // -ranges; empty <=> -d restores the whole text
    bool log = true; // false <=> -q
    direct_ofstream measurement_file; // -m
    std::string text_name; // the file name of <input_file> (compression) or of the output file (-d)
};

/**
 * @brief returns whether a symbol can occur in a sequence line of a FASTA file: a letter (a nucleotide or amino acid
 *        code), '*' (stop codon), '-' or '.' (gap), or '\r' (Windows line end)
 * @param c a symbol
 */
inline bool is_fasta_sequence_symbol(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '*' || c == '-' || c == '.' || c == '\r';
}

/**
 * @brief returns whether a file starts like a FASTA file: with fasta_header_start, and the first line that is no
 *        header line consists of sequence symbols only (see is_fasta_sequence_symbol)
 * @param path the path of the file
 */
inline bool starts_like_fasta(const std::string& path)
{
    direct_ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || !line.starts_with(fasta_header_start)) return false;

    while (std::getline(in, line)) {
        if (line.starts_with(fasta_header_start)) continue;
        return std::all_of(line.begin(), line.end(), is_fasta_sequence_symbol);
    }

    return true;
}

/**
 * @brief prints the help of a compressor: the common options, then the options of the tool
 * @param out the stream the help is written to
 * @param spec the compressor
 * @param print_options print_options(h) prints the options of the tool with the help_printer h
 */
template <typename print_options_fnc_t>
void print_compressor_help(std::ostream& out, const compressor_spec& spec, print_options_fnc_t print_options)
{
    const std::string name(spec.name);
    const std::string file_ending = "." + std::string(spec.ending);
    const bool with_postcompressor = !spec.default_postcompressor.empty();
    const std::string ending = file_ending + (with_postcompressor ? ".<postcompressor>" : "");
    const std::string ending_pattern = file_ending + (with_postcompressor ? "[.<postcompressor>]" : "");
    help_printer h(out, compressor_help_width);
    h.intro(name + ": " + std::string(spec.description) + ".", name + " [options] <input_file>");
    h.option("-d", "", "decompress <input_file> (*" + ending_pattern + ")");

    if (spec.ranges) {
        h.option("-ranges <file>", "a path", "with -d: write only these ranges of the text, one '<position> <length>' "
            "per line (the first position is 0)" + std::string(spec.fasta ? "; of the sequence text if the file was "
            "compressed in the FASTA mode" : ""));
    }

    h.option("-o <base_name>", "a path (default: <input_file>)", "write <base_name>" + ending
        + (with_postcompressor ? " (<base_name>" + file_ending + " with -pc none)" : "") + "; with -d: the output file "
        "(default: <input_file> without " + ending_pattern + ")");
    h.option("-t <threads>", "integer >= 1 (default: all)", "number of threads");

    if (with_postcompressor) {
        std::string names; // the names of the postcompressors, separated by " | "
        std::string qualities; // the range and the default of -<quality> of each postcompressor

        for (const postcompressor_spec& pc : postcompressors) {
            names += std::string(names.empty() ? "" : " | ") + pc.name;
            if (pc.kind == postcompressor_kind::none) continue;
            qualities += std::string(qualities.empty() ? "" : ", ") + pc.name + " [" + std::to_string(pc.min_level)
                + "," + std::to_string(pc.max_level) + "] (" + (pc.default_level == 0
                    ? std::string("from the peak memory usage") : std::to_string(pc.default_level)) + ")";
        }

        h.option("-pc <postcompressor>", names + " (default: " + std::string(spec.default_postcompressor) + ")",
            "postcompressor (it has to be on the PATH); none: no post-compression");
        h.option("-<quality>", "integer in a range and with a default per postcompressor: " + qualities,
            "post-compression quality, e.g. -9: higher gives smaller output but takes longer; for bsc and bzip3 the "
            "block size in MB (bsc default: the largest block that needs no more memory than the construction)");
    }

    if (spec.fasta) {
        h.option("-fasta <mode>", "on | off | auto (default: auto)", "store the header lines and the line layout of "
            "FASTA files separately from the sequences; auto: on for files that start with '"
            + std::string(1, fasta_header_start) + "' and whose first sequence line consists of letters, '*', '-' and "
            "'.' only");
    }

    h.option("-q", "", "print nothing");
    h.option("-v", "", "print progress and statistics (default)");
    h.option("-m <m_file>", "a path", "append results to <m_file>");
    h.option("-h", "", "show help");
    print_options(h);
}

/**
 * @brief reads the arguments of a compressor: the options, then <input_file>
 * @param argc the number of command-line arguments
 * @param argv the command-line arguments
 * @param spec the compressor
 * @param read_option read_option(option, args) reads an option of the tool (and its values from args) and returns
 *        whether it is one
 * @param cli the options to read
 * @throws usage_error if the command line is invalid; std::runtime_error if the postcompressor is not installed or the
 *         measurement file cannot be opened
 */
template <typename read_option_fnc_t>
void read_compressor_cli(int argc, char** argv, const compressor_spec& spec, read_option_fnc_t read_option,
    compressor_cli_t& cli)
{
    const bool with_postcompressor = !spec.default_postcompressor.empty();
    cli_arguments args(argc, argv);
    std::string postcompressor(spec.default_postcompressor); // -pc
    std::string path_output; // -o
    std::string path_measurement_file; // -m
    std::string fasta_mode = "auto"; // -fasta
    bool verbose = false; // -v

    // the last argument is <input_file>
    while (args.remaining() > 1) {
        const std::string s = args.next();

        if (s == "-d") {
            cli.decompress = true;
        } else if (s == "-o") {
            path_output = args.value(s);
        } else if (s == "-t") {
            cli.threads = args.integer(s, 1, omp_get_max_threads());
        } else if (s == "-q") {
            if (verbose) throw usage_error("-q and -v cannot be combined");
            cli.log = false;
        } else if (s == "-v") {
            if (!cli.log) throw usage_error("-q and -v cannot be combined");
            verbose = true;
        } else if (s == "-m") {
            path_measurement_file = args.value(s);
        } else if (spec.ranges && s == "-ranges") {
            cli.path_ranges_file = args.value(s);
        } else if (spec.fasta && s == "-fasta") {
            fasta_mode = args.value(s);
            if (fasta_mode != "on" && fasta_mode != "off" && fasta_mode != "auto")
                throw usage_error("-fasta expects on, off or auto");
        } else if (with_postcompressor && s == "-pc") {
            postcompressor = args.value(s);
        } else if (with_postcompressor && s.size() > 1
                && std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isdigit((unsigned char) c); })) {
            cli.quality = parse_integer_option("-<quality>", s.substr(1));
            cli.quality_given = true;
        } else if (!read_option(s, args)) {
            throw unknown_option(s);
        }
    }

    if (args.empty()) throw usage_error("missing <input_file>");
    cli.path_input_file = args.next();
    if (!std::filesystem::exists(cli.path_input_file)) throw usage_error(cli.path_input_file + " does not exist");
    if (!cli.path_ranges_file.empty() && !cli.decompress) throw usage_error("-ranges requires -d");
    const std::string ending = "." + std::string(spec.ending);

    if (cli.decompress) {
        // <input_file> without its endings; empty <=> it does not end in them
        std::string stripped;

        if (cli.path_input_file.ends_with(ending)) {
            stripped = cli.path_input_file.substr(0, cli.path_input_file.size() - ending.size());
            if (with_postcompressor) cli.postcompressor = find_postcompressor("none");
        } else if (with_postcompressor) {
            const size_t dot = cli.path_input_file.find_last_of('.');
            const std::string head = dot == std::string::npos ? "" : cli.path_input_file.substr(0, dot);
            const postcompressor_spec* pc = dot == std::string::npos ? nullptr
                : find_postcompressor(cli.path_input_file.substr(dot + 1));

            if (pc != nullptr && pc->kind != postcompressor_kind::none && head.ends_with(ending)) {
                stripped = head.substr(0, head.size() - ending.size());
                cli.postcompressor = pc;
            }
        }

        if (with_postcompressor && cli.postcompressor == nullptr)
            throw usage_error(cli.path_input_file + " ends neither in " + ending + " nor in " + ending
                + ".<postcompressor>");

        cli.path_output_file = path_output.empty() ? stripped : path_output;

        if (cli.path_output_file.empty())
            throw usage_error(cli.path_input_file + " does not end in " + ending + "; give the output file with -o");
    } else {
        if (with_postcompressor) {
            cli.postcompressor = find_postcompressor(postcompressor);
            if (cli.postcompressor == nullptr) throw usage_error("unknown postcompressor " + postcompressor);
            const postcompressor_spec& pc = *cli.postcompressor;

            if (cli.quality_given && pc.kind == postcompressor_kind::none)
                throw usage_error("-<quality> cannot be used with -pc none");

            if (cli.quality_given && (cli.quality < pc.min_level || cli.quality > pc.max_level)) {
                throw usage_error("-<quality> of " + postcompressor + " expects an integer in ["
                    + std::to_string(pc.min_level) + ", " + std::to_string(pc.max_level) + "]");
            }

            if (!cli.quality_given) cli.quality = pc.default_level;
        }

        cli.fasta = fasta_mode == "on" || (fasta_mode == "auto" && spec.fasta
            && starts_like_fasta(cli.path_input_file));
        const bool postcompressed = cli.postcompressor != nullptr
            && cli.postcompressor->kind != postcompressor_kind::none;
        cli.path_output_file = (path_output.empty() ? cli.path_input_file : path_output) + ending
            + (postcompressed ? "." + std::string(cli.postcompressor->name) : "");
    }

    if (std::filesystem::weakly_canonical(cli.path_output_file)
            == std::filesystem::weakly_canonical(cli.path_input_file))
        throw usage_error("the output file must differ from <input_file>");

    if (cli.postcompressor != nullptr && !postcompressor_installed(*cli.postcompressor)) {
        throw std::runtime_error("the postcompressor " + std::string(cli.postcompressor->name) + " is not installed ('"
            + cli.postcompressor->binary + "' is not on the PATH)" + (cli.decompress ? "" : "; choose another one "
            "with -pc"));
    }

    if (!path_measurement_file.empty()) {
        if (!open_measurement_file(cli.measurement_file, path_measurement_file))
            throw std::runtime_error("cannot open or create " + path_measurement_file);
        cli.text_name = std::filesystem::path(cli.decompress ? cli.path_output_file : cli.path_input_file)
            .filename().string();
    }
}

/**
 * @brief runs a compressor: compresses <input_file> or, with -d, restores it (see the file comment); -q writes
 *        nothing to the standard output; the errors are written to the standard error (see run_cli)
 * @param argc the number of command-line arguments
 * @param argv the command-line arguments
 * @param spec the compressor
 * @param print_options print_options(h) prints the options of the tool (see print_compressor_help)
 * @param read_option read_option(option, args) reads an option of the tool (see read_compressor_cli)
 * @param compress compress(cli) compresses cli.path_input_file into cli.path_output_file
 * @param decompress decompress(cli) restores the text of cli.path_input_file into cli.path_output_file
 * @return the exit code
 */
template <typename print_options_fnc_t, typename read_option_fnc_t, typename compress_fnc_t,
    typename decompress_fnc_t>
int run_compressor_cli(int argc, char** argv, const compressor_spec& spec, print_options_fnc_t print_options,
    read_option_fnc_t read_option, compress_fnc_t compress, decompress_fnc_t decompress)
{
    // the tool runs a postcompressor through itself to measure its peak memory usage (see run_postcompressor)
    if (!spec.default_postcompressor.empty() && argc > 1 && std::string_view(argv[1]) == measure_peak_option)
        return run_and_write_peak(argc, argv);

    auto print_help = [&](std::ostream& out) { print_compressor_help(out, spec, print_options); };

    return run_cli(argc, argv, print_help, [&] {
        compressor_cli_t cli;
        read_compressor_cli(argc, argv, spec, read_option, cli);
        omp_set_num_threads(cli.threads);
        if (!cli.log) std::cout.setstate(std::ios::badbit);
        if (cli.decompress) decompress(cli);
        else compress(cli);
        if (cli.measurement_file.is_open()) cli.measurement_file.close();
    });
}

/**
 * @brief logs the size of the output file of a compressor and the compression ratio:
 *        "output size: <size>" and "compression ratio = <input_bytes / output_bytes>"
 * @param output_bytes the size of the output file in bytes
 * @param input_bytes the size of the input file in bytes
 */
inline void log_compression_ratio(uint64_t output_bytes, uint64_t input_bytes)
{
    log_out() << "output size: " << format_size(output_bytes) << std::endl;
    log_out() << "compression ratio = " << format_ratio(input_bytes / (double) std::max<uint64_t>(1, output_bytes))
              << std::endl;
}

/** @brief the time, the peak memory usage and the setting of a post-compression */
struct postcompression_t {
    uint64_t time_ns = 0; // the time in nanoseconds
    uint64_t peak = 0; // the peak memory usage of the postcompressor in bytes
    uint64_t level = 0; // the level passed to the postcompressor; for bsc the block size in bytes
};

/**
 * @brief compresses a file with the postcompressor of -pc into the output file (precondition: it is not none)
 * @param cli the options
 * @param path_archive the file to compress
 * @param archive_bytes the size of the file in bytes
 * @param construction_peak the peak memory usage of the construction in bytes (see bsc_block_bytes)
 * @return the time, the peak memory usage and the setting
 * @throws std::runtime_error if the postcompressor fails
 */
inline postcompression_t postcompress(const compressor_cli_t& cli, const std::string& path_archive,
    uint64_t archive_bytes, uint64_t construction_peak)
{
    const postcompressor_spec& pc = *cli.postcompressor;
    postcompression_t result;
    result.level = cli.quality;

    if (pc.kind == postcompressor_kind::bsc)
        result.level = cli.quality_given ? cli.quality << 20 : bsc_block_bytes(construction_peak);

    const std::string setting = pc.kind == postcompressor_kind::bsc ? "block size " + format_size(result.level)
        : pc.kind == postcompressor_kind::bzip3 ? "block size "
            + std::to_string(bzip3_block_mb(archive_bytes, result.level)) + " MB"
        : "level " + std::to_string(result.level);
    temporary_files tmp_files;
    const std::string path_errors = temporary_file_path("postcompressor-errors-", cli.path_output_file);
    const std::string path_peak = temporary_file_path("postcompressor-peak-", cli.path_output_file);
    tmp_files.paths = { path_errors, path_peak };

    // remove an existing output file (7z would add the archive to it)
    std::error_code ec;
    std::filesystem::remove(cli.path_output_file, ec);
    auto time = now();
    log_phase_start(cli.log, time, "compressing with " + std::string(pc.name) + " (" + setting + ")");

    if (!run_postcompressor(compress_command(pc, path_archive, archive_bytes, cli.path_output_file, cli.threads,
            result.level), path_archive, cli.path_output_file, path_errors, cli.log, path_peak, &result.peak)) {
        std::filesystem::remove(cli.path_output_file, ec);
        const std::string errors = postcompressor_errors(path_errors);
        throw std::runtime_error(std::string(pc.name) + " failed" + (errors.empty() ? "" : ":\n" + errors));
    }

    result.time_ns = time_diff_ns(time, now());
    log_phase_end(cli.log, time);
    return result;
}

/**
 * @brief reads a file of ranges of the text, one "<position> <length>" per line (empty lines are skipped)
 * @param path the path of the file
 * @return the ranges (position, length)
 * @throws std::runtime_error if the file cannot be read, a line is invalid or there is no range
 */
inline std::vector<extract_range_t> read_ranges_file(const std::string& path)
{
    direct_ifstream in(path);
    require_readable(in, path);
    std::vector<extract_range_t> ranges;
    std::string line;

    for (uint64_t line_number = 1; std::getline(in, line); line_number++) {
        if (line.empty()) continue;
        const std::string where = " (line " + std::to_string(line_number) + " of " + path + ")";
        std::istringstream fields(line);
        std::string position, length;
        if (!(fields >> position)) throw std::runtime_error("missing <position>" + where);
        if (!(fields >> length)) throw std::runtime_error("missing <length>" + where);

        try {
            ranges.emplace_back(parse_integer_option("<position>", position),
                parse_integer_option("<length>", length));
        } catch (const usage_error& error) {
            throw std::runtime_error(error.what() + where);
        }
    }

    if (ranges.empty()) throw std::runtime_error("no ranges in " + path);
    return ranges;
}

/**
 * @brief restores the text of a file written by a compressor that stores a parsing (or the ranges of -ranges) into
 *        the output file: decompresses the file with its postcompressor into a temporary file next to the output
 *        file, reads the parsing with decompress() or load() and extracts the text. A compressor with a FASTA mode
 *        stores the mode (text_mode_plain or text_mode_fasta) in the first byte, in the FASTA mode followed by the
 *        FASTA section (see fasta_section) and the parsing of the sequence text, which is empty if nothing follows;
 *        -d restores the FASTA file, -ranges extracts ranges of the sequence text.
 * @tparam parsing_t the type of the parsing
 * @param cli the options
 * @param result_type the value of the key "type" of the RESULT line (see -m)
 * @param with_mode whether the file starts with the mode (the compressor has a FASTA mode)
 */
template <typename parsing_t>
void decompress_parsing(compressor_cli_t& cli, const std::string& result_type, bool with_mode = false)
{
    std::error_code ec;
    const uint64_t input_bytes = std::filesystem::file_size(cli.path_input_file, ec);
    if (ec) throw std::runtime_error("cannot read " + cli.path_input_file);
    const bool postcompressed = cli.postcompressor != nullptr
        && cli.postcompressor->kind != postcompressor_kind::none;

    log_out() << "decompressing " << cli.path_input_file << " (" << format_size(input_bytes) << ") using "
              << format_threads(cli.threads) << std::endl;
    log_out() << "output: " << cli.path_output_file << std::endl << std::endl;

    const uint64_t baseline = malloc_count_current();
    malloc_count_reset_peak();
    const auto time_start = now();
    auto time = now();
    parsing_t parsing;
    std::optional<fasta_section> fasta; // the FASTA section, in the FASTA mode
    bool empty = false; // whether the file stores no parsing (an empty sequence text in the FASTA mode)

    // true <=> parsing_t has no decompress(), i.e., the file stores the serialized parsing
    constexpr bool loads = !requires(parsing_t& x, direct_ifstream& in, uint64_t threads) {
        x.decompress(in, threads);
    };

    {
        temporary_files tmp_files;
        std::string path_archive = cli.path_input_file; // the file the parsing is read from

        if (postcompressed) {
            const postcompressor_spec& pc = *cli.postcompressor;
            path_archive = temporary_file_path("postcompressor-output-", cli.path_output_file);
            const std::string path_errors = temporary_file_path("postcompressor-errors-", cli.path_output_file);
            tmp_files.paths = { path_archive, path_errors };
            log_phase_start(cli.log, time, "decompressing with " + std::string(pc.name));

            if (!run_postcompressor(decompress_command(pc, cli.path_input_file, path_archive, cli.threads),
                    cli.path_input_file, path_archive, path_errors, cli.log)) {
                throw std::runtime_error(std::string(pc.name) + " failed on " + cli.path_input_file + ": "
                    + postcompressor_errors(path_errors));
            }

            log_phase_end(cli.log, time);
        }

        log_phase_start(cli.log, time, loads ? "loading the parsing" : "decompressing the parsing");
        direct_ifstream in(path_archive);
        require_readable(in, path_archive);

        if (with_mode) {
            stream_byte_reader reader(in);
            const uint8_t mode = reader.byte();
            if (mode != text_mode_plain && mode != text_mode_fasta) throw invalid_file();
            if (mode == text_mode_fasta) fasta = fasta_section::read(reader);
            empty = fasta && in.peek() == std::istream::traits_type::eof();
        }

        if (!empty) {
            if constexpr (!loads) parsing.decompress(in, cli.threads);
            else if constexpr (requires { parsing.load(in, cli.threads); }) parsing.load(in, cli.threads);
            else parsing.load(in);
        }

        log_phase_end(cli.log, time);
    }

    const uint64_t n = empty ? 0 : parsing.size();
    if (n == 0 && !fasta) throw std::runtime_error("the text in " + cli.path_input_file + " is empty");
    // whether the FASTA file is restored
    const bool restores_fasta = fasta && cli.path_ranges_file.empty();
    std::vector<extract_range_t> ranges;
    if (n > 0) ranges = { { 0, n } };
    if (!cli.path_ranges_file.empty()) ranges = read_ranges_file(cli.path_ranges_file);
    uint64_t num_symbols = 0; // the total number of symbols to extract

    // cut the ranges at the end of the text (a length of 0 counts as 1)
    for (auto& [position, length] : ranges) {
        if (position >= n) {
            throw std::runtime_error("position " + std::to_string(position) + " is past the end of the text (n = "
                + format_number(n) + ")");
        }

        length = std::min(n - position, std::max<uint64_t>(1, length));
        if (length > std::numeric_limits<uint64_t>::max() - num_symbols)
            throw std::runtime_error("the requested output is too large");
        num_symbols += length;
    }

    log_phase_start(cli.log, time, "extracting " + format_size(num_symbols)
        + (restores_fasta ? " and restoring the FASTA file" : ""));
    phase_progress progress(cli.log, num_symbols);

    if (restores_fasta) {
        if constexpr (extracts_phrase_by_phrase<parsing_t>) {
            throw std::runtime_error("the FASTA mode needs a parsing that is extracted window by window");
        } else {
            fasta_output out(cli.path_output_file);
            fasta_restorer restorer(*fasta, out);
            extract_windows(parsing, ranges, cli.threads, progress,
                [&](const char* symbols, uint64_t length) { restorer.append(symbols, length); });
            restorer.finish();
            out.finish();
        }
    } else if constexpr (extracts_phrase_by_phrase<parsing_t>) {
        extract_phrases_to_file(parsing, ranges, num_symbols, cli.path_output_file, cli.threads, progress);
    } else {
        direct_ofstream out(cli.path_output_file);
        require_created(out, cli.path_output_file);
        extract_windows(parsing, ranges, out, false, cli.threads, progress);
        out.close();
        if (!out.good()) throw std::runtime_error("cannot write " + cli.path_output_file);
    }

    progress.finish();
    log_phase_end(cli.log, time);
    const uint64_t time_ns = time_diff_ns(time_start, now());
    const uint64_t peak = overall_peak() - baseline;

    log_out() << std::endl << "decompression time: " << format_time(time_ns) << " ("
              << format_throughput(num_symbols, time_ns) << ")" << std::endl;
    if constexpr (is_malloc_count_linked) log_peak_memory_usage("peak memory usage", peak, num_symbols);

    if (cli.measurement_file.is_open()) {
        result_line(cli.measurement_file)
            ("type", result_type)
            ("text", cli.text_name)
            ("n", n)
            ("num_threads", cli.threads)
            ("time_decompress", time_ns)
            ("peak_memory_usage", peak);
    }
}

#endif // STRING_UTILS_MISC_COMPRESSOR_CLI_HPP
