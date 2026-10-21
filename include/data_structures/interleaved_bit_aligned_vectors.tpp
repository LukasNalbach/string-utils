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

#ifndef STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_TPP
#define STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_TPP

#include <data_structures/interleaved_bit_aligned_vectors.hpp>

inline void interleaved_bit_aligned_vectors::initialize(std::array<uint8_t, num_vectors> widths)
{
    size_vectors = 0;
    capacity_vectors = 0;
    width_entry = 0;

    data_vectors.clear();
    data_vectors.shrink_to_fit();

    for (uint8_t vec_idx = 0; vec_idx < num_vectors; vec_idx++) {
        if (widths[vec_idx] == 0) {
            this->widths[vec_idx] = 0;
            offsets[vec_idx] = 0;
            masks[vec_idx] = 0;
            fits_in_word[vec_idx] = false;
            word_masks[vec_idx] = 0;
        } else {
            offsets[vec_idx] = width_entry;
            this->widths[vec_idx] = widths[vec_idx];
            width_entry += widths[vec_idx];
            masks[vec_idx] = (uint128_t{1} << widths[vec_idx]) - 1;
            fits_in_word[vec_idx] = widths[vec_idx] <= max_word_access_width;
            word_masks[vec_idx] = fits_in_word[vec_idx] ? (uint64_t{1} << widths[vec_idx]) - 1 : 0;
        }
    }
}

inline void interleaved_bit_aligned_vectors::reserve(uint64_t capacity, uint64_t num_threads)
{
    if (capacity_vectors < capacity) {
        uint64_t num_old_bytes = byte_size(capacity_vectors);
        uint64_t num_new_bytes = byte_size(capacity);

        std::vector<char> new_data_vectors;
        ::resize_no_init(new_data_vectors, num_new_bytes + padding);
        advise_huge_pages(new_data_vectors.data(), num_new_bytes + padding);

        // set the padding to 0 (the reserved entries are not initialized)
        for (uint64_t i = num_new_bytes; i < num_new_bytes + padding; i++) {
            new_data_vectors[i] = 0;
        }

        #pragma omp parallel for num_threads(num_threads)
        for (uint64_t i = 0; i < num_old_bytes; i++) {
            new_data_vectors[i] = data_vectors[i];
        }

        std::swap(data_vectors, new_data_vectors);
        capacity_vectors = capacity;
    }
}

inline void interleaved_bit_aligned_vectors::resize(uint64_t size, uint64_t num_threads)
{
    if (capacity_vectors < size) {
        reserve(size, num_threads);
    }

    uint64_t num_old_bytes = byte_size(size_vectors);
    uint64_t num_new_bytes = byte_size(size);

    // set the bits after the last old entry in its 64-bit word to 0
    uint64_t old_bits = size_vectors * width_entry;

    if (size > size_vectors && old_bits % 64 != 0) {
        uint64_t* words = reinterpret_cast<uint64_t*>(data_vectors.data());
        words[old_bits / 64] &= (uint64_t { 1 } << (old_bits % 64)) - 1;
    }

    #pragma omp parallel for num_threads(num_threads)
    for (uint64_t i = num_old_bytes; i < num_new_bytes; i++) {
        data_vectors[i] = 0;
    }

    size_vectors = size;
}

inline void interleaved_bit_aligned_vectors::shrink_to_fit()
{
    if (size_vectors < capacity_vectors) {
        capacity_vectors = std::max<uint64_t>(2, size_vectors);
        data_vectors.resize(byte_size(capacity_vectors) + padding);
        data_vectors.shrink_to_fit();
        advise_huge_pages(data_vectors.data(), byte_size(capacity_vectors) + padding);
    }
}

template <uint8_t vec_idx>
inline void interleaved_bit_aligned_vectors::set_width(uint8_t width, uint64_t num_threads)
{
    static_assert(vec_idx < num_vectors);
    if (widths[vec_idx] == width) return;

    std::array<uint8_t, num_vectors> new_widths = widths;
    new_widths[vec_idx] = width;

    interleaved_bit_aligned_vectors resized(new_widths);
    resized.resize_no_init(size_vectors, num_threads);

    // copy the entries of all vectors with a new width > 0
    #pragma omp parallel for num_threads(num_threads)
    for (uint64_t i = 0; i < size_vectors; i++) {
        for_constexpr<0, num_vectors, 1>([&](auto j) {
            if (new_widths[j] != 0) resized.template set_parallel<j>(i, get<j>(i));
        });
    }

    *this = std::move(resized);
}

template <uint8_t vec_idx, typename T>
inline T interleaved_bit_aligned_vectors::get(uint64_t i) const
{
    auto [byte_idx, bit_offs] = bit_position<vec_idx>(i);

    // an entry of a vector with fits_in_word[vec_idx] = 1 is read as a single 64-bit word
    if constexpr (sizeof(uint128_t) > sizeof(uint64_t)) {
        if (fits_in_word[vec_idx]) {
            uint64_t block;
            std::memcpy(&block, &data_vectors[byte_idx], sizeof(uint64_t));
            return T((block >> bit_offs) & word_masks[vec_idx]);
        }
    }

    return T((read_block(byte_idx) >> bit_offs) & masks[vec_idx]);
}

template <uint64_t count, uint8_t vec_idx, typename val_t>
inline std::array<val_t, count> interleaved_bit_aligned_vectors::get_multiple(uint64_t i) const
{
    static_assert(count >= 1 && vec_idx + count <= num_vectors);

    auto [byte_idx, bit_offs] = bit_position<vec_idx>(i);
    std::array<uint8_t, count> field_widths;
    uint64_t span = 0; // sum of the widths of the count vectors

    for (uint64_t k = 0; k < count; k++) {
        field_widths[k] = widths[vec_idx + k];
        span += field_widths[k];
    }

    // splits bits (starting with the entry of vector vec_idx at bit 0) into the count entries
    auto split = [&](auto bits) {
        std::array<val_t, count> out;

        for (uint64_t k = 0; k < count; k++) {
            out[k] = val_t(bits & ((decltype(bits)(1) << field_widths[k]) - 1));
            bits >>= field_widths[k];
        }

        return out;
    };

    if (span + 7 <= 64) {
        uint64_t block;
        std::memcpy(&block, &data_vectors[byte_idx], sizeof(uint64_t));
        return split(block >> bit_offs);
    }

    // the entries do not fit into one uint128_t block; read each one separately
    if (span + 7 > 8 * sizeof(uint128_t)) {
        std::array<val_t, count> out;
        [&]<uint64_t... k>(std::index_sequence<k...>) {
            ((out[k] = get<uint8_t(vec_idx + k), val_t>(i)), ...);
        }(std::make_index_sequence<count>{});
        return out;
    }

    return split(read_block(byte_idx) >> bit_offs);
}

template <uint8_t vec_idx, typename T>
inline void interleaved_bit_aligned_vectors::set(uint64_t i, T v)
{
    auto [byte_idx, bit_offs] = bit_position<vec_idx>(i);

    // an entry of a vector with fits_in_word[vec_idx] = 1 is read and written as a single 64-bit word
    if constexpr (sizeof(uint128_t) > sizeof(uint64_t)) {
        if (fits_in_word[vec_idx]) {
            uint64_t block;
            std::memcpy(&block, &data_vectors[byte_idx], sizeof(uint64_t));
            block = (block & ~(word_masks[vec_idx] << bit_offs)) |
                ((uint64_t(v) & word_masks[vec_idx]) << bit_offs);
            std::memcpy(&data_vectors[byte_idx], &block, sizeof(uint64_t));
            return;
        }
    }

    write_block(byte_idx,
        (read_block(byte_idx) & ~(masks[vec_idx] << bit_offs)) |
        ((uint128_t(v) & masks[vec_idx]) << bit_offs));
}

template <uint8_t vec_idx, typename T>
inline void interleaved_bit_aligned_vectors::set_parallel(uint64_t i, T v)
{
    static_assert(vec_idx < num_vectors);

    // an entry of <= 64 bits spans at most two 64-bit words; write the bits in each word with atomic_store_bits
    uint64_t bit_pos = i * width_entry + offsets[vec_idx];
    uint64_t width = widths[vec_idx];
    uint64_t val = uint64_t(uint64_t(v)) & uint64_t(masks[vec_idx]);

    uint64_t* words = reinterpret_cast<uint64_t*>(data_vectors.data());
    uint64_t word_idx = bit_pos >> 6; // bit_pos / 64
    uint64_t bit_offs = bit_pos & 63; // bit_pos % 64
    uint64_t bits_lo = std::min<uint64_t>(64 - bit_offs, width); // bits stored in the first word

    uint64_t mask_lo = low_bits_mask(bits_lo) << bit_offs;
    atomic_store_bits(words[word_idx], mask_lo, (val << bit_offs) & mask_lo);

    if (bits_lo < width) { // the entry continues in the next 64-bit word
        uint64_t mask_hi = (uint64_t{1} << (width - bits_lo)) - 1;
        atomic_store_bits(words[word_idx + 1], mask_hi, (val >> bits_lo) & mask_hi);
    }
}

inline void interleaved_bit_aligned_vectors::emplace_back(std::array<uint64_t, num_vectors> vals)
{
    if (size_vectors == capacity_vectors) {
        reserve(size_vectors + std::max<uint64_t>(1, size_vectors / 2));
    }

    for_constexpr<0, num_vectors, 1>([&](auto vec_idx) {
        set<vec_idx>(size_vectors, vals[vec_idx]);
    });

    size_vectors++;
}

template <uint8_t vec_idx, typename T>
inline void interleaved_bit_aligned_vectors::emplace_back(T val)
{
    if (size_vectors == capacity_vectors) {
        reserve(size_vectors + std::max<uint64_t>(1, size_vectors / 2));
    }

    for_constexpr<0, num_vectors, 1>([&](auto j) {
        if constexpr (j == vec_idx) set<vec_idx, T>(size_vectors, val);
        else if (widths[j] != 0) set<j>(size_vectors, uint64_t { 0 });
    });

    size_vectors++;
}

inline void interleaved_bit_aligned_vectors::serialize(std::ostream& out) const
{
    out.write((char*) &size_vectors, sizeof(uint64_t));
    out.write((char*) widths.data(), sizeof(widths));

    if (size_vectors > 0) {
        // write only the bytes storing entries; the unused bits of the last 64-bit word are written as 0
        uint64_t bytes = byte_size(size_vectors);
        uint64_t used_bits = (size_vectors * width_entry) % 64;

        if (used_bits == 0) {
            write_in_chunks(out, data_vectors.data(), bytes);
        } else {
            write_in_chunks(out, data_vectors.data(), bytes - sizeof(uint64_t));
            uint64_t last;
            std::memcpy(&last, data_vectors.data() + bytes - sizeof(uint64_t), sizeof(uint64_t));
            last &= (uint64_t { 1 } << used_bits) - 1;
            out.write((char*) &last, sizeof(uint64_t));
        }
    }
}

inline void interleaved_bit_aligned_vectors::load(std::istream& in)
{
    uint64_t loaded_size;
    in.read((char*) &loaded_size, sizeof(uint64_t));
    in.read((char*) widths.data(), sizeof(widths));
    initialize(widths);

    if (loaded_size > 0) {
        resize_no_init(loaded_size);
        read_in_chunks(in, data_vectors.data(), byte_size(loaded_size));
    }
}

#endif // STRING_UTILS_DATA_STRUCTURES_INTERLEAVED_BIT_ALIGNED_VECTORS_TPP
