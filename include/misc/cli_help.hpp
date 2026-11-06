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
    void intro(std::string_view description, std::string_view usage);

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
    void option(std::string_view name, std::string_view values, std::string_view explanation = "");

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
    void wrap(std::string_view words, uint64_t indent);

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
    uint64_t max = std::numeric_limits<uint64_t>::max());

/**
 * @brief parses the value of an option as a decimal number (independently of the locale, see parse_double)
 * @param option the option, e.g. "-e"
 * @param text the value
 * @param min the smallest allowed value
 * @param max the largest allowed value
 * @return the value
 * @throws usage_error "<option> expects a number in [<min>, <max>]"
 */
inline double parse_number_option(const std::string& option, std::string_view text, double min, double max);

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
inline fasta_option read_fasta_option(cli_arguments& args);

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
inline bool starts_like_fasta(const std::string& path);

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
int run_cli(int argc, char** argv, help_fnc_t print_help, run_fnc_t run);

#include <misc/cli_help.tpp>

#endif // STRING_UTILS_MISC_CLI_HELP_HPP
