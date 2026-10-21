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
 * @file measurement.hpp
 * @brief the measurement file of the command-line tools and its RESULT lines
 */

#ifndef STRING_UTILS_MISC_MEASUREMENT_HPP
#define STRING_UTILS_MISC_MEASUREMENT_HPP

#include <filesystem>
#include <iostream>
#include <string>

#include <misc/files.hpp>

/**
 * @brief opens the measurement file given after -m, appending to it if it already exists
 * @param mf the stream to open
 * @param path the path of the measurement file
 * @return whether the file could be opened
 */
inline bool open_measurement_file(direct_ofstream& mf, const std::string& path)
{
    mf.open(path, std::filesystem::exists(path) ? std::ios::app : std::ios::out);
    return mf.good();
}

/**
 * @brief writes one measurement line of the form "RESULT key=value key=value ..."; the line is ended when
 *        the object is destroyed. Usage: result_line(mf)("algo", "count")("n", n);
 */
class result_line {
protected:
    std::ostream& out; // the stream the line is written to

public:
    // begins a RESULT line on the stream out
    explicit result_line(std::ostream& out) : out(out) { out << "RESULT"; }

    // ends the line
    ~result_line() { out << std::endl; }

    result_line(const result_line&) = delete;
    result_line& operator=(const result_line&) = delete;

    /**
     * @brief appends one key-value pair to the line
     * @tparam val_t type of the value
     * @param key the key
     * @param value the value
     * @return a reference to this line
     */
    template <typename val_t>
    result_line& operator()(const char* key, const val_t& value)
    {
        out << ' ' << key << '=' << value;
        return *this;
    }
};

#endif // STRING_UTILS_MISC_MEASUREMENT_HPP
