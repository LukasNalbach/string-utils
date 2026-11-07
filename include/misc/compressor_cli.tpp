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
 * @file compressor_cli.tpp
 * @brief the command-line interface of the compressors: help, options, compressing and extracting
 */

#ifndef STRING_UTILS_MISC_COMPRESSOR_CLI_TPP
#define STRING_UTILS_MISC_COMPRESSOR_CLI_TPP

#include <misc/compressor_cli.hpp>

template <typename print_options_fnc_t>
inline void print_compressor_help(std::ostream& out, const compressor_spec& spec, print_options_fnc_t print_options)
{
    const std::string name(spec.name);
    const std::string file_ending = "." + std::string(spec.ending);
    const bool with_postcompressor = !spec.default_postcompressor.empty();
    const std::string ending = file_ending + (with_postcompressor ? ".<postcompressor>" : "");
    const std::string ending_pattern = file_ending + (with_postcompressor ? "[.<postcompressor>]" : "");
    help_printer h(out);
    h.intro(name + ": " + std::string(spec.description) + ".", name + " [options] <input_file>");
    h.group("arguments");
    h.option("<input_file>", "", "the file to compress; with -d a file *" + ending_pattern);
    h.group("input");
    h.option("-d", "", "decompress <input_file>");

    if (spec.fasta) {
        h.option_fasta("store the header lines and the line layout of FASTA files separately from the sequences; "
            "-d reads the mode from <input_file>");
    }

    print_options(h, compressor_help_part::tool);
    h.group("output");
    h.option("-o <base_name>", "a path (default: <input_file>)", "write <base_name>" + ending
        + (with_postcompressor ? " (<base_name>" + file_ending + " with -pc none)" : "") + "; with -d: the output file "
        "(default: <input_file> without " + ending_pattern + ")");

    if (spec.ranges) {
        h.option("-ranges <file>", "a path", "with -d: write only these ranges of the restored file, one '<position> "
            "<length>' per line (the first position is 0)");
    }

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

    h.group("execution");
    h.option_threads();
    print_options(h, compressor_help_part::execution);
    h.option("-q", "", "print nothing");
    h.group("measurement");
    h.option_measurement();
}

template <typename read_option_fnc_t>
inline void read_compressor_cli(int argc, char** argv, const compressor_spec& spec, read_option_fnc_t read_option, compressor_cli_t& cli)
{
    const bool with_postcompressor = !spec.default_postcompressor.empty();
    cli_arguments args(argc, argv);
    std::string postcompressor(spec.default_postcompressor); // -pc
    std::string path_output; // -o
    fasta_option fasta_mode = fasta_option::detect; // -fasta

    // the last argument is <input_file>
    while (args.remaining() > 1) {
        const std::string s = args.next();

        if (s == "-d") {
            cli.decompress = true;
        } else if (s == "-o") {
            path_output = args.value(s);
        } else if (s == "-p") {
            cli.threads = args.integer(s, 1, omp_get_max_threads());
        } else if (s == "-q") {
            cli.log = false;
        } else if (s == "-m") {
            read_measurement_option(args, cli.measurement_file, cli.text_name);
        } else if (spec.ranges && s == "-ranges") {
            cli.path_ranges_file = args.value(s);
        } else if (spec.fasta && s == "-fasta") {
            fasta_mode = read_fasta_option(args);
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

    if (args.empty()) throw usage_error("expected <input_file>");
    cli.path_input_file = args.next();
    if (!std::filesystem::exists(cli.path_input_file)) throw usage_error(cli.path_input_file + " does not exist");
    if (!cli.path_ranges_file.empty() && !cli.decompress) throw usage_error("-ranges needs -d");
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

        cli.fasta = spec.fasta && uses_fasta_mode(fasta_mode, cli.path_input_file);
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
}

template <typename print_options_fnc_t, typename read_option_fnc_t, typename compress_fnc_t,
    typename decompress_fnc_t>
inline int run_compressor_cli(int argc, char** argv, const compressor_spec& spec, print_options_fnc_t print_options,
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

template <typename parsing_t>
inline void write_fasta(const parsing_t& parsing, uint64_t n, const fasta_section& section, const fasta_position_map* map,
    const std::vector<extract_range_t>& ranges, fasta_output& out, const std::string& sibling_file, uint64_t p,
    phase_progress& progress)
{
    // the ranges of the sequence text that are extracted
    const std::vector<extract_range_t> text_ranges = map != nullptr ? map->text_ranges(ranges)
        : std::vector<extract_range_t>(n > 0 ? 1 : 0, extract_range_t(0, n));
    uint64_t num_symbols = 0;
    for (const extract_range_t& range : text_ranges) num_symbols += range.second;
    progress.set_total(num_symbols);

    if (map != nullptr) {
        fasta_range_writer writer(*map, out, ranges);
        extract_windows_in_order(parsing, text_ranges, sibling_file, p, progress,
            [&](const char* symbols, uint64_t length) { writer.append(symbols, length); });
        writer.finish();
    } else {
        fasta_restorer restorer(section, out);
        extract_windows_in_order(parsing, text_ranges, sibling_file, p, progress,
            [&](const char* symbols, uint64_t length) { restorer.append(symbols, length); });
        restorer.finish();
    }
}

#endif // STRING_UTILS_MISC_COMPRESSOR_CLI_TPP
