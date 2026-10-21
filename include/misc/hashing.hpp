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
 * @file hashing.hpp
 * @brief hash functions on 64-bit values (splitmix64) and the arithmetic modulo the Mersenne prime 2^61 - 1 that
 *        Karp-Rabin fingerprints are computed with
 */

#ifndef STRING_UTILS_MISC_HASHING_HPP
#define STRING_UTILS_MISC_HASHING_HPP

#include <cstdint>

#include <misc/packed_integers.hpp>

// floor(2^64 / phi), where phi is the golden ratio; the increment of splitmix64
inline constexpr uint64_t golden_ratio_64 = 0x9e3779b97f4a7c15ULL;

/**
 * @brief returns the output function of splitmix64 applied to a 64-bit value (a bijection on 64-bit values)
 * @param x a 64-bit value
 * @return the mixed value
 */
inline constexpr uint64_t mix_64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/**
 * @brief hashes a 64-bit value with splitmix64: mix_64(x + golden_ratio_64)
 * @param x a 64-bit value
 * @return the hash of x
 */
inline constexpr uint64_t hash_64(uint64_t x) { return mix_64(x + golden_ratio_64); }

// arithmetic modulo the Mersenne prime 2^61 - 1; a value x is reduced by adding its bits above bit 60 to its low 61
// bits, which leaves x mod (2^61 - 1) unchanged because 2^61 = 1 (mod 2^61 - 1)
namespace mersenne_61 {

inline constexpr uint64_t exponent = 61;
inline constexpr uint64_t prime = (uint64_t { 1 } << exponent) - 1; // 2^61 - 1

// returns x mod prime for any 64-bit value x
inline uint64_t reduce(uint64_t x)
{
    x = (x & prime) + (x >> exponent);
    return x >= prime ? x - prime : x;
}

// returns x mod prime for x < 2^124
inline uint64_t reduce_wide(uint128_t x) { return reduce(uint64_t(x & prime) + uint64_t(x >> exponent)); }

// returns (a * b) mod prime for a, b in [0..prime-1]
inline uint64_t mul(uint64_t a, uint64_t b)
{
    const uint128_t product = uint128_t(a) * uint128_t(b);
    const uint64_t folded = (uint64_t(product) & prime) + uint64_t(product >> exponent);
    return folded >= prime ? folded - prime : folded;
}

// returns (a + b) mod prime for a, b in [0..prime-1]
inline uint64_t add(uint64_t a, uint64_t b)
{
    const uint64_t sum = a + b;
    return sum >= prime ? sum - prime : sum;
}

// returns (a - b) mod prime for a, b in [0..prime-1]
inline uint64_t sub(uint64_t a, uint64_t b) { return a >= b ? a - b : prime - (b - a); }

} // namespace mersenne_61

#endif // STRING_UTILS_MISC_HASHING_HPP
