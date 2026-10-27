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

#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#include <gtest/gtest.h>
#include <omp.h>

#include <coding/byte_codes.hpp>
#include "test-fasta.hpp"
#include "test-fuzz.hpp"

namespace {

std::mt19937_64 gen(test_seed());

// the largest length of the sequences of a random FASTA file; adjusted by run_fuzz
uint64_t max_input_size = 20000;
// smallest value run_fuzz reduces max_input_size to
constexpr uint64_t min_input_size = 64;

// a text with random access (see extract_range)
struct plain_text {
    std::string T;

    uint64_t size() const { return T.size(); }
    void extract(uint64_t position, uint64_t length, char* out) const { std::memcpy(out, T.data() + position, length); }
};

} // namespace

TEST(test_string_utils, fasta)
{
    run_fuzz("fasta", {
        { "random-fasta", [](uint64_t) {
            const std::string file = random_fasta_file(max_input_size, num_byte_values - 1, gen);
            verify_fasta_file(file, std::uniform_int_distribution<uint64_t>(1, omp_get_max_threads())(gen),
                [](const std::string& T) { return plain_text { T }; }, gen);
        }, false }
    }, fuzz_iterations(), &max_input_size, min_input_size);
}
