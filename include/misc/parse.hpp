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

#ifndef STRING_UTILS_MISC_PARSE_HPP
#define STRING_UTILS_MISC_PARSE_HPP

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <system_error>

/**
 * @brief parses a decimal number with a point as the decimal separator, independently of the locale (std::stod and
 *        std::strtod use the separator of LC_NUMERIC)
 * @param text the number
 * @param value set to the number, if text is one
 * @return whether text is a number without further characters
 */
inline bool parse_double(std::string_view text, double& value)
{
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    return !text.empty() && error == std::errc() && stop == end;
}

/**
 * @brief returns the value of an environment variable as an unsigned decimal integer
 * @param name name of the variable
 * @param fallback returned if the variable is not set or not an unsigned decimal integer
 */
inline uint64_t env_uint(const char* name, uint64_t fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const uint64_t value = std::strtoull(text, &end, 10);
    return end != text && *end == '\0' ? value : fallback;
}

/**
 * @brief returns the value of an environment variable as a decimal number (see parse_double)
 * @param name name of the variable
 * @param fallback returned if the variable is not set or not a decimal number
 */
inline double env_double(const char* name, double fallback)
{
    const char* text = std::getenv(name);
    double value = 0;
    return text != nullptr && parse_double(text, value) ? value : fallback;
}

#endif // STRING_UTILS_MISC_PARSE_HPP
