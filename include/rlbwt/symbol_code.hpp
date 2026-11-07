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
 * @file symbol_code.hpp
 * @brief the canonical Huffman code of the run symbols of the compressed format (see section_symbol_code)
 */

#ifndef STRING_UTILS_RLBWT_SYMBOL_CODE_HPP
#define STRING_UTILS_RLBWT_SYMBOL_CODE_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <functional>
#include <queue>

#include <rlbwt/bits.hpp>

namespace rlbwt {

/**
 * @brief a canonical Huffman code of the run symbols: the codes of length l are the consecutive integers
 *        first_code[l], first_code[l] + 1, ... assigned to the symbols of code length l in increasing order. A code is
 *        stored most significant bit first in the bit order of the stream (least significant bit of a byte first).
 *        Section layout: the largest code length L (64 bits), then for l in [1..L] the number of codes of length l
 *        (64 bits each), then the symbols in the order of their codes (symbol_bytes bytes each).
 */
class symbol_code {
public:
    // the largest code length; a code is read with read_bits
    static constexpr uint8_t max_code_bits = max_packed_bits;

    // codes of at most lookup_bits bits are decoded with one lookup in table
    static constexpr uint8_t lookup_bits = 11;

private:
    std::vector<uint64_t> symbols; // the symbols in the order of their codes
    std::vector<uint64_t> count; // [0..max_length] count[l] = number of codes of length l
    std::vector<uint64_t> first_code; // [0..max_length] first_code[l] = the smallest code of length l
    std::vector<uint64_t> first_index; // [0..max_length] first_index[l] = index in symbols of first_code[l]
    uint8_t max_length = 0; // the largest code length
    uint8_t table_bits = 0; // min(max_length, lookup_bits)
    /* [0..2^table_bits-1] table[x] = (index in symbols << byte_bits) | code length, if the code of length at most
       table_bits is a prefix of the next table_bits bits x of the stream; 0 otherwise */
    std::vector<uint64_t> table;
    // [0..symbols.size()-1] the code of each symbol, bit-reversed (in stream order), and its length
    std::vector<uint64_t> stream_code;
    std::vector<uint8_t> length;
    // the index in symbols of a symbol: in dense_index for symbols that fit into two bytes, else in sparse_index
    std::vector<uint64_t> dense_index;
    std::unordered_map<uint64_t, uint64_t> sparse_index;

    // computes first_code, first_index, table, stream_code and the indexes from symbols and count
    void initialize()
    {
        const uint64_t m = symbols.size();
        first_code.assign(max_length + 1, 0);
        first_index.assign(max_length + 1, 0);
        uint64_t code = 0, index = 0;

        for (uint8_t l = 1; l <= max_length; l++) {
            first_code[l] = code;
            first_index[l] = index;
            code = (code + count[l]) << 1;
            index += count[l];
        }

        stream_code.resize(m);
        length.resize(m);
        table_bits = std::min(max_length, lookup_bits);
        table.assign(uint64_t { 1 } << table_bits, 0);
        uint8_t l = 1;

        for (uint64_t i = 0; i < m; i++) {
            while (i >= first_index[l] + count[l]) l++;
            const uint64_t c = first_code[l] + (i - first_index[l]);
            uint64_t reversed = 0;
            for (uint8_t j = 0; j < l; j++) reversed |= ((c >> j) & 1) << (l - 1 - j);
            stream_code[i] = reversed;
            length[i] = l;

            if (l <= table_bits) {
                for (uint64_t f = 0; f < (uint64_t { 1 } << (table_bits - l)); f++) {
                    table[reversed | (f << l)] = (i << byte_bits) | l;
                }
            }
        }

        // the symbols that fit into two bytes are indexed by an array, the others by a hash table
        const uint64_t dense_limit = uint64_t { std::numeric_limits<uint16_t>::max() } + 1;
        uint64_t dense_size = 0;
        for (uint64_t sym : symbols) if (sym < dense_limit) dense_size = std::max(dense_size, sym + 1);
        dense_index.assign(dense_size, 0);
        sparse_index.clear();

        for (uint64_t i = 0; i < m; i++) {
            if (symbols[i] < dense_size) dense_index[symbols[i]] = i;
            else sparse_index.emplace(symbols[i], i);
        }
    }

    // returns the index of a symbol in symbols
    inline uint64_t index_of(uint64_t sym) const
    {
        return sym < dense_index.size() ? dense_index[sym] : sparse_index.at(sym);
    }

public:
    symbol_code() = default;

    /**
     * @brief computes the code from the frequencies of the symbols
     * @param frequencies the (symbol, frequency) pairs of the distinct symbols (frequencies >= 1, at least one pair)
     */
    explicit symbol_code(std::vector<std::pair<uint64_t, uint64_t>> frequencies)
    {
        const uint64_t m = frequencies.size();
        // [0..m-1] the code length of each symbol
        std::vector<uint8_t> lengths(m, 1);

        if (m > 1) {
            std::vector<uint64_t> weight(m);
            for (uint64_t i = 0; i < m; i++) weight[i] = frequencies[i].second;
            /* the nodes of the Huffman tree: [0..m-1] the leaves, [m..2m-2] the inner nodes in the order of their creation;
               parent[v] = the parent of node v, depth[v] = the depth of node v */
            std::vector<uint64_t> parent(2 * m - 1), depth(2 * m - 1);

            // a code longer than max_code_bits is avoided by halving the weights (rounded up) until none is longer
            while (true) {
                using node_t = std::pair<uint64_t, uint64_t>; // (weight, node)
                std::priority_queue<node_t, std::vector<node_t>, std::greater<node_t>> queue;
                for (uint64_t i = 0; i < m; i++) queue.emplace(weight[i], i);
                uint64_t next = m;

                while (queue.size() > 1) {
                    const node_t a = queue.top();
                    queue.pop();
                    const node_t b = queue.top();
                    queue.pop();
                    parent[a.second] = parent[b.second] = next;
                    queue.emplace(a.first + b.first, next++);
                }

                depth[2 * m - 2] = 0;
                for (uint64_t v = 2 * m - 2; v-- > 0;) depth[v] = depth[parent[v]] + 1;
                const uint64_t longest = *std::max_element(depth.begin(), depth.begin() + m);

                if (longest <= max_code_bits) {
                    for (uint64_t i = 0; i < m; i++) lengths[i] = uint8_t(depth[i]);
                    break;
                }

                for (uint64_t& w : weight) w = (w + 1) / 2;
            }
        }

        // the symbols in the order of their codes: by code length, then by symbol
        std::vector<uint64_t> order(m);
        for (uint64_t i = 0; i < m; i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
            return lengths[a] != lengths[b] ? lengths[a] < lengths[b] : frequencies[a].first < frequencies[b].first;
        });

        max_length = *std::max_element(lengths.begin(), lengths.end());
        count.assign(max_length + 1, 0);
        symbols.resize(m);

        for (uint64_t i = 0; i < m; i++) {
            symbols[i] = frequencies[order[i]].first;
            count[lengths[order[i]]]++;
        }

        initialize();
    }

    /**
     * @brief reads the code from its section
     * @param data the bytes of the section
     * @param size number of bytes of the section
     * @param symbol_bytes bytes per symbol
     * @return whether the section stores a valid code
     */
    bool load(const uint8_t* data, uint64_t size, uint8_t symbol_bytes)
    {
        if (size < sizeof(uint64_t)) return false;
        const uint64_t max_len = read_bytes(data, sizeof(uint64_t));
        if (max_len == 0 || max_len > max_code_bits || size < (max_len + 1) * sizeof(uint64_t)) return false;
        max_length = uint8_t(max_len);
        count.assign(max_length + 1, 0);
        uint64_t m = 0;
        // the codes satisfy the Kraft inequality: sum of 2^(max_length - l) over the codes <= 2^max_length
        uint64_t kraft = 0;

        for (uint8_t l = 1; l <= max_length; l++) {
            count[l] = read_bytes(data + l * sizeof(uint64_t), sizeof(uint64_t));
            if (count[l] > (uint64_t { 1 } << l)) return false;
            m += count[l];
            kraft += count[l] << (max_length - l);
        }

        const uint64_t header = (uint64_t { max_length } + 1) * sizeof(uint64_t);
        if (m == 0 || kraft > (uint64_t { 1 } << max_length) || (size - header) / symbol_bytes != m
            || (size - header) % symbol_bytes != 0) {
            return false;
        }

        symbols.resize(m);
        for (uint64_t i = 0; i < m; i++) symbols[i] = read_bytes(data + header + i * symbol_bytes, symbol_bytes);
        initialize();
        return true;
    }

    /**
     * @brief returns the bytes of the section of the code
     * @param symbol_bytes bytes per symbol
     */
    std::string serialize(uint8_t symbol_bytes) const
    {
        std::string bytes((uint64_t { max_length } + 1) * sizeof(uint64_t) + symbols.size() * symbol_bytes, 0);
        uint8_t* data = (uint8_t*) bytes.data();
        write_bytes(data, max_length, sizeof(uint64_t));
        for (uint8_t l = 1; l <= max_length; l++) write_bytes(data + l * sizeof(uint64_t), count[l], sizeof(uint64_t));
        const uint64_t header = (uint64_t { max_length } + 1) * sizeof(uint64_t);
        for (uint64_t i = 0; i < symbols.size(); i++) write_bytes(data + header + i * symbol_bytes, symbols[i], symbol_bytes);
        return bytes;
    }

    // returns the number of distinct symbols
    uint64_t num_symbols() const { return symbols.size(); }

    // returns the length of the code of a symbol
    inline uint8_t code_length(uint64_t sym) const { return length[index_of(sym)]; }

    // appends the code of a symbol to bits
    inline void encode(bit_buffer& bits, uint64_t sym) const
    {
        const uint64_t i = index_of(sym);
        bits.put(stream_code[i], length[i]);
    }

    /**
     * @brief decodes the code at a bit offset
     * @param data the bit-packed data (see read_bits)
     * @param bit offset (in bits) of the code; it is advanced by the code length
     * @return the symbol
     */
    inline uint64_t decode(const uint8_t* data, uint64_t& bit) const
    {
        const uint64_t window = read_bits(data, bit, max_length);
        const uint64_t entry = table[window & low_mask(table_bits)];

        if (entry != 0) [[likely]] {
            bit += entry & low_mask(byte_bits);
            return symbols[entry >> byte_bits];
        }

        uint64_t code = 0;

        for (uint8_t l = 1;; l++) {
            code = (code << 1) | ((window >> (l - 1)) & 1);

            if (code - first_code[l] < count[l]) {
                bit += l;
                return symbols[first_index[l] + code - first_code[l]];
            }
        }
    }

    // skips the code at a bit offset (see decode)
    inline void skip(const uint8_t* data, uint64_t& bit) const { decode(data, bit); }
};

} // namespace rlbwt

#endif // STRING_UTILS_RLBWT_SYMBOL_CODE_HPP
