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

#ifndef STRING_UTILS_MISC_TEMP_FILES_HPP
#define STRING_UTILS_MISC_TEMP_FILES_HPP

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <misc/platform.hpp>

/**
 * @brief generates a random string of alphanumeric characters
 * @param length number of characters in the generated string
 * @return a random string of the given length over the alphabet [0-9A-Za-z]
 */
inline std::string random_alphanumeric_string(uint64_t length)
{
    static const std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::random_device device;
    std::mt19937 gen(device());
    std::uniform_int_distribution<uint32_t> symbol_idx(0, alphabet.size() - 1);

    std::string result;
    result.reserve(length);

    for (uint64_t i = 0; i < length; i++) {
        result.push_back(alphabet[symbol_idx(gen)]);
    }

    return result;
}

/**
 * @brief returns the directory for temporary files: the value of the environment variable <prefix>_TMPDIR (see
 *        STRING_UTILS_ENV_PREFIX in misc/platform.hpp) if it is set, else std::filesystem::temp_directory_path()
 *        (TMPDIR or /tmp; on Windows TMP or TEMP); prints a warning (once per process) if the directory lies on a RAM
 *        disk (see platform::is_ram_disk)
 * @return the directory
 */
inline std::filesystem::path temporary_directory()
{
    static const std::filesystem::path dir = [] {
        std::filesystem::path tmp_dir;
        const char* env = std::getenv(STRING_UTILS_ENV_PREFIX "_TMPDIR");

        if (env != nullptr && env[0] != 0) {
            tmp_dir = std::filesystem::path(env);
        } else {
            std::error_code error;
            tmp_dir = std::filesystem::temp_directory_path(error);
            if (error) tmp_dir = std::filesystem::path(platform::fallback_temporary_directory);
        }

        if (platform::is_ram_disk(tmp_dir.string())) {
            std::cerr << "warning: the temporary directory " << tmp_dir.string()
                      << " is a RAM disk (temporary files are stored in memory);"
                      << " set " STRING_UTILS_ENV_PREFIX "_TMPDIR to a directory on a disk" << std::endl;
        }

        return tmp_dir;
    }();

    return dir;
}

/**
 * @brief returns a unique path for a temporary file (the file is not created)
 * @param prefix prefix of the file name; 10 random alphanumeric characters are appended to it
 * @param sibling_file path of a file; the temporary file is placed in its directory, or in the temporary
 *        directory (see temporary_directory) if sibling_file is empty or contains no directory
 * @return the path
 */
inline std::string temporary_file_path(const std::string& prefix, const std::string& sibling_file = "")
{
    const std::string name = prefix + random_alphanumeric_string(10);
    const std::filesystem::path dir = sibling_file.empty()
        ? std::filesystem::path() : std::filesystem::path(sibling_file).parent_path();
    return (dir.empty() ? temporary_directory() / name : dir / name).string();
}

// removes the files in paths when it is destroyed
struct temporary_files {
    std::vector<std::string> paths;

    temporary_files() = default;
    temporary_files(const temporary_files&) = delete;
    temporary_files& operator=(const temporary_files&) = delete;

    ~temporary_files()
    {
        for (const std::string& path : paths) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
};

#endif // STRING_UTILS_MISC_TEMP_FILES_HPP
