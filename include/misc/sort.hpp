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

#ifndef STRING_UTILS_MISC_SORT_HPP
#define STRING_UTILS_MISC_SORT_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <omp.h>

#include <ips2ra.hpp>
#include <ips4o.hpp>

#include <misc/utils.hpp>

// wrappers of ips4o and ips2ra; the instantiations in STRING_UTILS_SORT_INSTANTIATIONS are compiled in src/sort.cpp

// sorts [beg, end) by comp with ips4o::parallel::sort using num_threads threads
template <typename it_t, typename comp_t>
void sort_parallel(it_t beg, it_t end, comp_t comp, uint64_t num_threads)
{
    ips4o::parallel::sort(beg, end, comp, int(num_threads));
}

// sorts [beg, end) by comp with ips4o::sort (sequential)
template <typename it_t, typename comp_t>
void sort_sequential(it_t beg, it_t end, comp_t comp)
{
    ips4o::sort(beg, end, comp);
}

// sorts [beg, end) by key with ips2ra::parallel::sort using num_threads threads
template <typename it_t, typename key_t>
void radix_sort_parallel(it_t beg, it_t end, key_t key, uint64_t num_threads)
{
    ips2ra::parallel::sort(beg, end, key, int(num_threads));
}

// sorts [beg, end) by key with ips2ra::sort (sequential)
template <typename it_t, typename key_t>
void radix_sort_sequential(it_t beg, it_t end, key_t key)
{
    ips2ra::sort(beg, end, key);
}

// inputs with fewer elements are sorted with std::sort
inline constexpr uint64_t min_ips_sort_size = uint64_t { 1 } << 12;

// radix_sort and comparison_sort use at most one thread per this many elements
inline constexpr uint64_t elements_per_sort_thread = uint64_t { 1 } << 13;

// returns the number of threads radix_sort and comparison_sort use for n elements; 1 inside a parallel region (the
// parallel sorts of ips2ra and ips4o do not return there)
inline uint64_t num_sort_threads(uint64_t n, uint64_t num_threads)
{
    if (omp_in_parallel()) return 1;
    return std::min<uint64_t>(num_threads, std::max<uint64_t>(1, n / elements_per_sort_thread));
}

/**
 * @brief sorts [beg, end) by key: with std::sort if there are fewer than min_ips_sort_size elements, else with
 *        ips2ra (in-place radix sort) and at most one thread per elements_per_sort_thread elements
 * @tparam it_t iterator type
 * @tparam key_t type of the key function
 * @param beg iterator to the first element
 * @param end iterator past the last element
 * @param key function returning the unsigned integer key of an element
 * @param num_threads maximum number of threads to use
 */
template <typename it_t, typename key_t>
void radix_sort(it_t beg, it_t end, key_t key, uint64_t num_threads)
{
    const uint64_t n = uint64_t(end - beg);

    if (n < min_ips_sort_size) {
        std::sort(beg, end, [&](const auto& a, const auto& b) { return key(a) < key(b); });
        return;
    }

    num_threads = num_sort_threads(n, num_threads);
    if (num_threads > 1) radix_sort_parallel(beg, end, key, num_threads);
    else radix_sort_sequential(beg, end, key);
}

/**
 * @brief sorts [beg, end) by comp: with std::sort if there are fewer than min_ips_sort_size elements, else with
 *        ips4o and at most one thread per elements_per_sort_thread elements (for keys that are no unsigned integers)
 * @tparam it_t iterator type
 * @tparam comp_t type of the comparison function
 * @param beg iterator to the first element
 * @param end iterator past the last element
 * @param comp comparison function
 * @param num_threads maximum number of threads to use
 */
template <typename it_t, typename comp_t>
void comparison_sort(it_t beg, it_t end, comp_t comp, uint64_t num_threads)
{
    const uint64_t n = uint64_t(end - beg);

    if (n < min_ips_sort_size) {
        std::sort(beg, end, comp);
        return;
    }

    num_threads = num_sort_threads(n, num_threads);
    if (num_threads > 1) sort_parallel(beg, end, comp, num_threads);
    else sort_sequential(beg, end, comp);
}

// explicit instantiations of the sort functions for vectors of uint64_t; prefix is empty in src/sort.cpp (where
// they are compiled) and extern everywhere else
#define STRING_UTILS_SORT_INSTANTIATIONS(prefix) \
    prefix template void sort_parallel<std::vector<uint64_t>::iterator, std::less<>>( \
        std::vector<uint64_t>::iterator, std::vector<uint64_t>::iterator, std::less<>, uint64_t); \
    prefix template void sort_sequential<std::vector<uint64_t>::iterator, std::less<>>( \
        std::vector<uint64_t>::iterator, std::vector<uint64_t>::iterator, std::less<>);

// the editor parser of VS Code (__INTELLISENSE__) does not resolve the extern declarations of constrained member
// templates; it does not need them
#if !defined(STRING_UTILS_NO_EXTERN_TEMPLATES) && !defined(__INTELLISENSE__)
STRING_UTILS_SORT_INSTANTIATIONS(extern)
#endif

#endif // STRING_UTILS_MISC_SORT_HPP
