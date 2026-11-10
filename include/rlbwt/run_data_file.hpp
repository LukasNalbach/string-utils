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
 * @file run_data_file.hpp
 * @brief a runs file (see ext_runs): its table of contents, its fields and the parts that are kept in RAM for the
 *        readers (the record samples of the plain format; the block indexes and the symbol code of the compressed
 *        format)
 */

#ifndef STRING_UTILS_RLBWT_RUN_DATA_FILE_HPP
#define STRING_UTILS_RLBWT_RUN_DATA_FILE_HPP

#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include <misc/bits.hpp>
#include <misc/files.hpp>
#include <misc/memory.hpp>
#include <rlbwt/bits.hpp>
#include <rlbwt/symbol_code.hpp>
#include <rlbwt/types.hpp>

namespace rlbwt {

/**
 * @brief a runs file (see ext_runs): which sections it stores and where, its fields and the parts of it that are kept
 *        in RAM for the readers
 */
class run_data_file {
private:
    std::string path; // path of the file
    std::vector<run_data_entry> entries; // the entries of the table of contents, in their order in the file
    run_data_fields values {}; // the fields (see section_fields)
    file_format fmt = file_format::plain; // the format of the runs
    uint64_t blocks = 0; // number of blocks of runs (compressed) or of record samples (plain)
    uint64_t lcp_blocks = 0; // compressed: number of LCP blocks
    // plain: the section section_record_samples; compressed: the section section_run_block_index (both padded)
    no_init_vector<uint8_t> run_index;
    no_init_vector<uint8_t> lcp_index; // compressed: the section section_lcp_block_index (padded)
    symbol_code code; // compressed: the code of the run symbols
    uint64_t index_entry_bits = 0; // compressed: bits per entry of the section section_run_block_index

    // reads the bytes of a section into a buffer followed by read_padding bytes
    bool read_padded(uint64_t id, no_init_vector<uint8_t>& bytes) const
    {
        const run_data_entry* e = find(id);
        if (e == nullptr) return false;
        resize_no_init(bytes, e->size + read_padding);
        std::memset(bytes.data() + e->size, 0, read_padding);
        if (e->size == 0) return true;
        direct_reader file(path);
        std::memcpy(bytes.data(), file.read(e->offset, e->offset + e->size), e->size);
        return true;
    }

    // checks the fields and the sections and reads the parts kept in RAM
    bool read_contents()
    {
        std::string bytes;
        if (!read_section(section_fields, bytes) || bytes.size() != sizeof(run_data_fields)) return false;
        std::memcpy(&values, bytes.data(), sizeof(run_data_fields));
        const run_data_fields& f = values;
        if (f.r == 0 || f.r > f.n || f.runs_per_block == 0 || f.sa_samples > 1 || f.lcp_samples > 1) return false;
        blocks = div_ceil(f.r, f.runs_per_block);
        lcp_blocks = f.lcp_samples != 0 && f.lcp_values_per_block != 0 ? div_ceil(f.r, f.lcp_values_per_block) : 0;
        const bool plain = contains(section_records);
        fmt = plain ? file_format::plain : file_format::compressed;

        if (plain) {
            if (f.position_bytes == 0 || f.position_bytes > sizeof(uint64_t) || f.symbol_bytes == 0
                || f.symbol_bytes > sizeof(uint64_t)) {
                return false;
            }

            const record_layout layout(f);
            return section(section_records).size == f.r * layout.bytes && read_padded(section_record_samples, run_index)
                && run_index.size() == blocks * f.position_bytes + read_padding;
        }

        const uint64_t sa_bytes = div_ceil<uint64_t>(f.r * f.position_bits, byte_bits);
        if (f.position_bits == 0 || f.position_bits > max_packed_bits || f.run_block_offset_bits > max_packed_bits
            || f.lcp_block_offset_bits > max_packed_bits || f.runs_per_symbol_sample == 0
            || f.symbol_bytes == 0 || f.symbol_bytes > sizeof(uint64_t) || !contains(section_run_blocks)) {
            return false;
        }

        if (f.sa_samples != 0 && (!contains(section_sa_run_starts) || !contains(section_sa_run_ends)
            || section(section_sa_run_starts).size != sa_bytes || section(section_sa_run_ends).size != sa_bytes)) {
            return false;
        }

        if (f.lcp_samples != 0 && (f.lcp_values_per_block == 0 || !contains(section_lcp_blocks)
            || !read_padded(section_lcp_block_index, lcp_index)
            || lcp_index.size() != div_ceil<uint64_t>(lcp_blocks * f.lcp_block_offset_bits, byte_bits) + read_padding)) {
            return false;
        }

        index_entry_bits = f.run_block_offset_bits + f.position_bits;
        no_init_vector<uint8_t> code_bytes;
        return read_padded(section_run_block_index, run_index)
            && run_index.size() == div_ceil<uint64_t>(blocks * index_entry_bits, byte_bits) + read_padding
            && read_padded(section_symbol_code, code_bytes)
            && code.load(code_bytes.data(), code_bytes.size() - read_padding, uint8_t(f.symbol_bytes));
    }

public:
    run_data_file() = default;

    /**
     * @brief reads the table of contents, the fields and the parts kept in RAM of a file
     * @param file_name name of the file
     * @return whether the file ends with a table of contents whose sections have distinct identifiers and lie in the
     *         file before it, and stores the sections of one of the formats with fitting sizes
     */
    bool open(const std::string& file_name)
    {
        path = file_name;
        entries.clear();
        bool valid = false;

        try {
            direct_reader file(file_name);
            const uint64_t file_size = file.size();
            uint64_t num_entries = 0;
            if (file_size < sizeof(num_entries)) return false;
            std::memcpy(&num_entries, file.read(file_size - sizeof(num_entries), file_size), sizeof(num_entries));
            if (num_entries > (file_size - sizeof(num_entries)) / sizeof(run_data_entry)) return false;
            // the sections end before the table of contents
            const uint64_t contents_offset = file_size - sizeof(num_entries) - num_entries * sizeof(run_data_entry);
            entries.resize(num_entries);

            if (num_entries > 0) {
                std::memcpy(entries.data(), file.read(contents_offset, file_size - sizeof(num_entries)),
                    num_entries * sizeof(run_data_entry));
            }

            valid = true;

            for (uint64_t i = 0; valid && i < num_entries; i++) {
                valid = entries[i].offset <= contents_offset && entries[i].size <= contents_offset - entries[i].offset;
                for (uint64_t j = 0; valid && j < i; j++) valid = entries[j].id != entries[i].id;
            }

            valid = valid && read_contents();
        } catch (const std::exception&) {
            valid = false;
        }

        if (!valid) entries.clear();
        return valid;
    }

    // returns the name of the file
    const std::string& file() const { return path; }

    // returns the entries of the table of contents, in their order in the file
    const std::vector<run_data_entry>& sections() const { return entries; }

    // returns the entry of the section with identifier id, or nullptr if the file does not store it
    const run_data_entry* find(uint64_t id) const
    {
        for (const run_data_entry& e : entries) if (e.id == id) return &e;
        return nullptr;
    }

    // returns whether the file stores the section with identifier id
    bool contains(uint64_t id) const { return find(id) != nullptr; }

    // returns the entry of the section with identifier id; throws if the file does not store it
    const run_data_entry& section(uint64_t id) const
    {
        const run_data_entry* e = find(id);
        if (e == nullptr)
            throw std::runtime_error(path + " does not store the section " + std::to_string(id));
        return *e;
    }

    /**
     * @brief reads the bytes of a section (with O_DIRECT, see direct_reader)
     * @param id identifier of the section
     * @param bytes is set to the bytes
     * @return whether the file stores the section and it could be read
     */
    bool read_section(uint64_t id, std::string& bytes) const
    {
        const run_data_entry* e = find(id);
        if (e == nullptr) return false;
        bytes.clear();
        if (e->size == 0) return true;

        try {
            direct_reader file(path);
            const uint8_t* data = file.read(e->offset, e->offset + e->size);
            bytes.assign((const char*) data, e->size);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // returns the fields of the file
    const run_data_fields& fields() const { return values; }

    // returns the format of the runs
    file_format format() const { return fmt; }

    // returns the length n of T$
    uint64_t n() const { return values.n; }

    // returns the number r of runs
    uint64_t r() const { return values.r; }

    // returns whether the file stores the SA values at the starts and at the ends of the runs
    bool has_sa_samples() const { return values.sa_samples != 0; }

    // returns whether the file stores the LCP values at the starts of the runs
    bool has_lcp_samples() const { return values.lcp_samples != 0; }

    // returns the number of blocks of runs (compressed) or of record samples (plain): ceil(r / runs_per_block)
    uint64_t num_blocks() const { return blocks; }

    // returns the number of runs in block b
    uint64_t runs_in_block(uint64_t b) const
    {
        return std::min(values.runs_per_block, values.r - b * values.runs_per_block);
    }

    // returns the start of the first run of block b (n for b = num_blocks())
    uint64_t block_start(uint64_t b) const
    {
        if (b == blocks) return values.n;
        if (fmt == file_format::plain) return read_bytes(run_index.data() + b * values.position_bytes, values.position_bytes);
        return read_bits(run_index.data(), b * index_entry_bits + values.run_block_offset_bits, values.position_bits);
    }

    // returns the largest b such that block_start(b) <= j (j < n)
    uint64_t block_of_position(uint64_t j) const
    {
        uint64_t lo = 0, hi = blocks; // block_start(lo) <= j < block_start(hi)

        while (hi - lo > 1) {
            const uint64_t mid = lo + (hi - lo) / 2;
            if (block_start(mid) <= j) lo = mid;
            else hi = mid;
        }

        return lo;
    }

    // compressed: returns the offset (in bytes) of block b of runs in section_run_blocks (its size for num_blocks())
    uint64_t run_block_offset(uint64_t b) const
    {
        if (b == blocks) return section(section_run_blocks).size;
        return read_bits(run_index.data(), b * index_entry_bits, values.run_block_offset_bits);
    }

    // compressed: returns the number of LCP blocks: ceil(r / lcp_values_per_block) (0 if no LCP values are stored)
    uint64_t num_lcp_blocks() const { return lcp_blocks; }

    // compressed: returns the number of LCP values in LCP block b
    uint64_t lcp_values_in_block(uint64_t b) const
    {
        return std::min(values.lcp_values_per_block, values.r - b * values.lcp_values_per_block);
    }

    // compressed: returns the offset (in bytes) of LCP block b in section_lcp_blocks (its size for num_lcp_blocks())
    uint64_t lcp_block_offset(uint64_t b) const
    {
        if (b == lcp_blocks) return section(section_lcp_blocks).size;
        return read_bits(lcp_index.data(), b * values.lcp_block_offset_bits, values.lcp_block_offset_bits);
    }

    // compressed: returns the code of the run symbols
    const symbol_code& symbols() const { return code; }

    // returns the number of bytes of the parts kept in RAM
    uint64_t ram_bytes() const
    {
        return run_index.size() + lcp_index.size() + code.num_symbols() * (sizeof(uint64_t) + sizeof(uint8_t));
    }
};

} // namespace rlbwt

#endif // STRING_UTILS_RLBWT_RUN_DATA_FILE_HPP
