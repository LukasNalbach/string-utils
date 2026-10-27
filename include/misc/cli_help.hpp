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
 * @file cli_help.hpp
 * @brief the command-line interface of the tools: the help of a mode in one layout (a description, the usage line, the
 *        arguments and the options in groups, in the order arguments, input, what is stored or computed, encodings,
 *        output, execution, measurement; each option first lists the values it takes, then its explanation), reading
 *        the arguments with checked values, and running a mode with uniform error messages and exit codes
 */

#ifndef STRING_UTILS_MISC_CLI_HELP_HPP
#define STRING_UTILS_MISC_CLI_HELP_HPP

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <misc/log.hpp>
#include <misc/measurement.hpp>
#include <misc/parse.hpp>

// ############################# HELP #############################

// the default width of a line of a help
inline constexpr uint64_t help_width = 100;

// the column in which the values and the explanation of an option start
inline constexpr uint64_t help_column = 26;

// the indentation of an option
inline constexpr uint64_t help_indent = 3;

// the values of the option -p <threads> of every tool
inline constexpr std::string_view threads_values = "integer >= 1 (default: all CPUs the process may run on)";

// the first byte of a FASTA file (the start of its first header line)
inline constexpr char fasta_header_start = '>';

// the symbols kept verbatim in the FASTA mode if -A is not given (see help_printer::option_fasta_alphabet)
inline constexpr std::string_view fasta_default_alphabet = "ACGT";

/**
 * @brief formats a default value for a help, as an output stream writes it (e.g. 1, 0.05)
 * @param value the value
 * @return the formatted value
 */
inline std::string help_value(double value)
{
    std::ostringstream s;
    s << value;
    return s.str();
}

/** @brief prints the help of a mode (see the file comment) */
class help_printer {
public:
    /**
     * @param out the stream the help is written to
     * @param width the width of a line
     */
    explicit help_printer(std::ostream& out = std::cout, uint64_t width = help_width) : out(&out), width(width) {}

    /**
     * @brief prints the description of the mode and its usage line
     * @param description what the mode does (wrapped)
     * @param usage the usage line, e.g. "tool count [options] <index_file> <patterns_file>"
     */
    void intro(std::string_view description, std::string_view usage)
    {
        wrap(description, 0);
        new_line();
        new_line();
        *out << "usage: " << usage;
        new_line();
    }

    /**
     * @brief prints the heading of a group of options
     * @param name the name of the group
     */
    void group(std::string_view name)
    {
        new_line();
        *out << name << ':';
        new_line();
    }

    /**
     * @brief prints an option or an argument
     * @param name the option with its placeholder, e.g. "-p <threads>"
     * @param values the values it takes, e.g. "integer >= 1 (default: 1)"; empty for an option without a value
     * @param explanation what it does (wrapped; a '\n' starts a new paragraph)
     */
    void option(std::string_view name, std::string_view values, std::string_view explanation = "")
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

    // prints the option -p <threads> (the number of threads, see threads_values)
    void option_threads() { option("-p <threads>", threads_values); }

    // prints the option -m <file> <name> (see read_measurement_option)
    void option_measurement()
    {
        option("-m <file> <name>", "a path and a name", "append measurement data to this file; <name> names the text");
    }

    /**
     * @brief prints the option -fasta <mode> (see read_fasta_option)
     * @param explanation what the FASTA mode does
     */
    void option_fasta(const std::string& explanation)
    {
        option("-fasta <mode>", "on | off | auto (default: auto)", explanation + "\nauto: on for files that start "
            "with '" + std::string(1, fasta_header_start) + "' and whose first line after the header lines consists of "
            "letters, '*', '-' and '.' only");
    }

    // prints the option -A <alphabet> of the FASTA mode (see fasta_default_alphabet)
    void option_fasta_alphabet()
    {
        option("-A <alphabet>", "a string of symbols (default: " + std::string(fasta_default_alphabet) + ")",
            "in the FASTA mode: the symbols kept verbatim");
    }

    /**
     * @brief prints a paragraph (wrapped)
     * @param paragraph the paragraph
     */
    void text(std::string_view paragraph)
    {
        new_line();
        wrap(paragraph, 0);
        new_line();
    }

private:
    std::ostream* out; // the stream the help is written to
    uint64_t width; // the width of a line
    uint64_t column = 0; // the column of the next character on the current line

    /**
     * @brief writes words, wrapped at width; a continued line starts at indent
     * @param words the words, separated by spaces; a '\n' ends the line
     * @param indent the column at which continued lines start
     */
    void wrap(std::string_view words, uint64_t indent)
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

    // ends the current line
    void new_line()
    {
        *out << '\n';
        column = 0;
    }

    // pads the current line with spaces up to column c
    void pad_to(uint64_t c)
    {
        while (column < c) {
            *out << ' ';
            column++;
        }
    }
};

// ############################# ARGUMENTS #############################

/**
 * @brief an error in the command line: an unknown option, a missing or invalid value, or a wrong number of arguments
 *        (run_cli prints the help of the mode after the message)
 */
class usage_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief returns the error "unknown option <option>"
 * @param option the option
 */
inline usage_error unknown_option(const std::string& option) { return usage_error("unknown option " + option); }

/**
 * @brief parses the value of an option as an unsigned decimal integer
 * @param option the option, e.g. "-p"
 * @param text the value
 * @param min the smallest allowed value
 * @param max the largest allowed value
 * @return the value
 * @throws usage_error "<option> expects an integer >= <min>" or "<option> expects an integer in [<min>, <max>]"
 */
inline uint64_t parse_integer_option(const std::string& option, std::string_view text, uint64_t min = 0,
    uint64_t max = std::numeric_limits<uint64_t>::max())
{
    uint64_t value = 0;
    const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (text.empty() || error != std::errc() || stop != text.data() + text.size() || value < min || value > max) {
        throw usage_error(option + " expects an integer " + (max == std::numeric_limits<uint64_t>::max()
            ? ">= " + std::to_string(min) : "in [" + std::to_string(min) + ", " + std::to_string(max) + "]"));
    }

    return value;
}

/**
 * @brief parses the value of an option as a decimal number (independently of the locale, see parse_double)
 * @param option the option, e.g. "-e"
 * @param text the value
 * @param min the smallest allowed value
 * @param max the largest allowed value
 * @return the value
 * @throws usage_error "<option> expects a number in [<min>, <max>]"
 */
inline double parse_number_option(const std::string& option, std::string_view text, double min, double max)
{
    double value = 0;

    if (!parse_double(text, value) || !(value >= min && value <= max))
        throw usage_error(option + " expects a number in [" + help_value(min) + ", " + help_value(max) + "]");

    return value;
}

/** @brief reads the arguments of a mode from left to right */
class cli_arguments {
public:
    /**
     * @param argc the number of command-line arguments
     * @param argv the command-line arguments; argv[0] is the name of the mode (or of the tool)
     */
    cli_arguments(int argc, char** argv) : argc(argc), argv(argv) {}

    // returns whether all arguments have been read
    bool empty() const { return pos >= argc; }

    // returns the number of arguments that have not been read
    uint64_t remaining() const { return pos >= argc ? 0 : uint64_t(argc - pos); }

    // returns whether the next argument is an option (it starts with '-' and is not "-")
    bool at_option() const { return pos < argc && argv[pos][0] == '-' && argv[pos][1] != '\0'; }

    // returns the next argument and advances to the one after it (precondition: !empty())
    std::string next() { return argv[pos++]; }

    /**
     * @brief returns the value of an option, the next argument, and advances to the one after it
     * @param option the option (for the error message)
     * @throws usage_error "missing value after <option>"
     */
    std::string value(const std::string& option)
    {
        if (empty()) throw usage_error("missing value after " + option);
        return next();
    }

    /**
     * @brief returns the value of an option as an unsigned decimal integer (see parse_integer_option)
     * @param option the option (for the error message)
     * @param min the smallest allowed value
     * @param max the largest allowed value
     */
    uint64_t integer(const std::string& option, uint64_t min = 0, uint64_t max = std::numeric_limits<uint64_t>::max())
    {
        return parse_integer_option(option, value(option), min, max);
    }

    /**
     * @brief returns the value of an option as a decimal number (see parse_number_option)
     * @param option the option (for the error message)
     * @param min the smallest allowed value
     * @param max the largest allowed value
     */
    double number(const std::string& option, double min, double max)
    {
        return parse_number_option(option, value(option), min, max);
    }

private:
    int argc; // the number of command-line arguments
    char** argv; // the command-line arguments
    int pos = 1; // the index of the next argument in argv
};

/**
 * @brief reads the values of the option -m <file> <name>: opens the measurement file (see open_measurement_file)
 * @param args the arguments, positioned after -m
 * @param file the stream to open
 * @param text_name is set to <name>
 * @throws usage_error if a value is missing; std::runtime_error "cannot open or create <file>"
 */
inline void read_measurement_option(cli_arguments& args, direct_ofstream& file, std::string& text_name)
{
    const std::string path = args.value("-m");
    text_name = args.value("-m");
    if (!open_measurement_file(file, path)) throw std::runtime_error("cannot open or create " + path);
}

/** @brief the values of the option -fasta <mode> */
enum class fasta_option { on, off, detect };

/**
 * @brief reads the value of the option -fasta <mode>
 * @param args the arguments, positioned after -fasta
 * @throws usage_error "-fasta expects on, off or auto"
 */
inline fasta_option read_fasta_option(cli_arguments& args)
{
    const std::string mode = args.value("-fasta");
    if (mode == "on") return fasta_option::on;
    if (mode == "off") return fasta_option::off;
    if (mode == "auto") return fasta_option::detect;
    throw usage_error("-fasta expects on, off or auto");
}

/**
 * @brief reads the value of an option that is switched on or off
 * @param args the arguments, positioned after the option
 * @param option the option (for the error message)
 * @return whether the option is switched on
 * @throws usage_error "<option> expects on or off"
 */
inline bool read_on_off(cli_arguments& args, const std::string& option)
{
    const std::string value = args.value(option);
    if (value == "on") return true;
    if (value == "off") return false;
    throw usage_error(option + " expects on or off");
}

/**
 * @brief returns whether a symbol can occur in a line of a FASTA file that is no header line: a letter, '*', '-',
 *        '.' or '\r' (the end of a line with the line break "\r\n")
 * @param c a symbol
 */
inline bool is_fasta_line_symbol(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '*' || c == '-' || c == '.' || c == '\r';
}

/**
 * @brief returns whether a file starts like a FASTA file: with fasta_header_start, and the first line that is no
 *        header line consists of the symbols of is_fasta_line_symbol only; only the first
 *        direct_input_filebuf::buffer_length bytes of the file are read
 * @param path the path of the file
 */
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

/**
 * @brief returns whether a file is read in the FASTA mode
 * @param mode the value of -fasta
 * @param path the path of the file
 */
inline bool uses_fasta_mode(fasta_option mode, const std::string& path)
{
    return mode == fasta_option::on || (mode == fasta_option::detect && starts_like_fasta(path));
}

/**
 * @brief checks that an input file was opened
 * @param stream the stream of the file
 * @param path the path of the file
 * @throws std::runtime_error "cannot read <path>"
 */
template <typename stream_t>
void require_readable(const stream_t& stream, const std::string& path)
{
    if (!stream.good()) throw std::runtime_error("cannot read " + path);
}

/**
 * @brief checks that an output file was created
 * @param stream the stream of the file
 * @param path the path of the file
 * @throws std::runtime_error "cannot create <path>"
 */
template <typename stream_t>
void require_created(const stream_t& stream, const std::string& path)
{
    if (!stream.good()) throw std::runtime_error("cannot create " + path);
}

// ############################# RUNNING A MODE #############################

/**
 * @brief runs a mode of a tool: without arguments or with -h or --help, it prints the help to stdout (exit code 0);
 *        a usage_error is printed to stderr as "error: <message>", followed by the help, any other exception as
 *        "error: <message>" (exit code 1); an open phase line is ended first (see end_phase_line)
 * @tparam help_fnc_t type of print_help
 * @tparam run_fnc_t type of run
 * @param argc the number of command-line arguments
 * @param argv the command-line arguments; argv[0] is the name of the mode
 * @param print_help print_help(out) prints the help of the mode to the stream out
 * @param run run() reads the arguments and runs the mode
 * @return the exit code
 */
template <typename help_fnc_t, typename run_fnc_t>
int run_cli(int argc, char** argv, help_fnc_t print_help, run_fnc_t run)
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

#endif // STRING_UTILS_MISC_CLI_HELP_HPP
