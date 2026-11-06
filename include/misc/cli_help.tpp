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

#ifndef STRING_UTILS_MISC_CLI_HELP_TPP
#define STRING_UTILS_MISC_CLI_HELP_TPP

#include <misc/cli_help.hpp>

inline void help_printer::intro(std::string_view description, std::string_view usage)
{
    wrap(description, 0);
    new_line();
    new_line();
    *out << "usage: " << usage;
    new_line();
}

inline void help_printer::option(std::string_view name, std::string_view values, std::string_view explanation)
{
    pad_to(help_indent);
    *out << name;
    column += name.size();

    if (column + 1 > help_column) new_line();
    pad_to(help_column);
    if (!values.empty()) {
        wrap(values, help_column);
        if (!explanation.empty()) new_line();
    }

    if (!explanation.empty()) {
        pad_to(help_column);
        wrap(explanation, help_column);
    }

    new_line();
}

inline void help_printer::wrap(std::string_view words, uint64_t indent)
{
    uint64_t i = 0;

    while (i < words.size()) {
        if (words[i] == '\n') {
            new_line();
            pad_to(indent);
            i++;
            continue;
        }

        if (words[i] == ' ') {
            i++;
            continue;
        }

        uint64_t j = i;
        while (j < words.size() && words[j] != ' ' && words[j] != '\n') j++;
        const uint64_t length = j - i;
        const bool first = column <= indent;

        if (!first && column + 1 + length > width) {
            new_line();
            pad_to(indent);
        } else if (!first) {
            *out << ' ';
            column++;
        }

        *out << words.substr(i, length);
        column += length;
        i = j;
    }
}

inline uint64_t parse_integer_option(const std::string& option, std::string_view text, uint64_t min, uint64_t max)
{
    uint64_t value = 0;
    const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (text.empty() || error != std::errc() || stop != text.data() + text.size() || value < min || value > max) {
        throw usage_error(option + " expects an integer " + (max == std::numeric_limits<uint64_t>::max()
            ? ">= " + std::to_string(min) : "in [" + std::to_string(min) + ", " + std::to_string(max) + "]"));
    }

    return value;
}

inline double parse_number_option(const std::string& option, std::string_view text, double min, double max)
{
    double value = 0;

    if (!parse_double(text, value) || !(value >= min && value <= max))
        throw usage_error(option + " expects a number in [" + help_value(min) + ", " + help_value(max) + "]");

    return value;
}

inline fasta_option read_fasta_option(cli_arguments& args)
{
    const std::string mode = args.value("-fasta");
    if (mode == "on") return fasta_option::on;
    if (mode == "off") return fasta_option::off;
    if (mode == "auto") return fasta_option::detect;
    throw usage_error("-fasta expects on, off or auto");
}

inline bool starts_like_fasta(const std::string& path)
{
    constexpr uint64_t max_bytes = direct_input_filebuf::buffer_length;
    direct_ifstream in(path, std::ios_base::in, max_bytes);
    std::string start(max_bytes, '\0');
    in.read(start.data(), std::streamsize(max_bytes));
    start.resize(uint64_t(in.gcount()));
    if (!start.starts_with(fasta_header_start)) return false;
    uint64_t line_start = 0; // the start of the current line in start

    while (line_start < start.size()) {
        const uint64_t line_end = std::min(start.find('\n', line_start), start.size());

        if (start[line_start] != fasta_header_start) {
            return std::all_of(start.begin() + line_start, start.begin() + line_end, is_fasta_line_symbol);
        }

        line_start = line_end + 1;
    }

    return true;
}

template <typename help_fnc_t, typename run_fnc_t>
inline int run_cli(int argc, char** argv, help_fnc_t print_help, run_fnc_t run)
{
    if (argc < 2) {
        print_help(std::cout);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (std::string_view(argv[i]) == "-h" || std::string_view(argv[i]) == "--help") {
            print_help(std::cout);
            return 0;
        }
    }

    try {
        run();
        return 0;
    } catch (const usage_error& error) {
        end_phase_line();
        std::cerr << "error: " << error.what() << "\n\n";
        print_help(std::cerr);
    } catch (const std::exception& error) {
        end_phase_line();
        std::cerr << "error: " << error.what() << std::endl;
    }

    return 1;
}

#endif // STRING_UTILS_MISC_CLI_HELP_TPP
