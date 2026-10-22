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
 * @file layout_model.hpp
 * @brief the context models of the line layout of a FASTA file: the lengths of the sequence lines of each record
 */

#ifndef STRING_UTILS_FASTA_LAYOUT_MODEL_HPP
#define STRING_UTILS_FASTA_LAYOUT_MODEL_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/integer_model.hpp>
#include <coding/range_coder.hpp>

// the lengths of the lines of a record, run-length encoded: [i] = (length, number of consecutive lines)
using fasta_line_runs = std::vector<std::pair<uint64_t, uint64_t>>;

/**
 * @brief codes the line layout of the records of a FASTA file. The sequence lines of a record with sequence length
 *        L are regular with a width W >= 1 if there are ceil(L / W) of them and all except the last one have length
 *        W; empty lines at the end of a record are counted separately. Per record: whether its lines are regular
 *        (context: the decision of the previous record); if they are and L > 0, whether W is the width of the
 *        previous record (context: the decision of the previous record), else W - 1; if they are not, the number of
 *        runs of equal line lengths minus one and each run (length, number of lines minus one); then the number of
 *        empty lines at the end (context: whether the previous record ended with empty lines).
 */
class fasta_layout_model {
private:
    // number of contexts of a model whose context is a decision of the previous record
    static constexpr uint64_t yes_no = 2;

    decision_model irregular {yes_no}; // the models of "the lines are not regular"
    decision_model new_width {yes_no}; // the models of "W is not the width of the previous record"
    integer_model widths {1}; // the model of W - 1
    integer_model num_runs {1}; // the model of the number of runs of an irregular record minus one
    integer_model run_lengths {1}; // the model of the line length of a run
    integer_model run_counts {1}; // the model of the number of lines of a run minus one
    integer_model empty_lines {yes_no}; // the models of the number of empty lines at the end of a record
    uint64_t width = 0; // the width of the last record with regular lines and L > 0 (0: none)
    bool last_irregular = false; // whether the lines of the previous record are not regular
    bool last_new_width = false; // whether the previous record has a new width
    bool last_empty_lines = false; // whether the previous record ends with empty lines

    /**
     * @brief returns whether the lines of a record are regular with a width
     * @param L the sequence length of the record (> 0)
     * @param body the runs of the lines without the empty lines at the end
     * @param w the width
     */
    static bool is_regular(uint64_t L, const fasta_line_runs& body, uint64_t w)
    {
        if (w == 0) return false;
        const uint64_t k = (L - 1) / w + 1; // the number of lines
        const uint64_t last = L - (k - 1) * w; // the length of the last line
        if (k == 1) return body.size() == 1 && body[0] == std::make_pair(L, uint64_t(1));
        if (last == w) return body.size() == 1 && body[0] == std::make_pair(w, k);
        return body.size() == 2 && body[0] == std::make_pair(w, k - 1) && body[1] == std::make_pair(last, uint64_t(1));
    }

public:
    /**
     * @brief encodes the lines of a record
     * @param coder the encoder
     * @param L the sequence length of the record (the sum of its line lengths)
     * @param lines the runs of its sequence lines (runs of equal lengths are merged)
     */
    void encode(range_encoder& coder, uint64_t L, fasta_line_runs lines)
    {
        uint64_t empty = 0;

        if (!lines.empty() && lines.back().first == 0) {
            empty = lines.back().second;
            lines.pop_back();
        }

        const bool same = L > 0 && is_regular(L, lines, width);
        const bool regular = L == 0 || same || is_regular(L, lines, lines.front().first);
        irregular.code(coder, !regular, last_irregular);
        last_irregular = !regular;

        if (regular && L > 0) {
            new_width.code(coder, !same, last_new_width);
            last_new_width = !same;

            if (!same) {
                width = lines.front().first;
                widths.code(coder, width - 1, 0);
            }
        } else if (!regular) {
            num_runs.code(coder, lines.size() - 1, 0);

            for (const auto& [length, count] : lines) {
                run_lengths.code(coder, length, 0);
                run_counts.code(coder, count - 1, 0);
            }
        }

        empty_lines.code(coder, empty, last_empty_lines);
        last_empty_lines = empty > 0;
    }

    /**
     * @brief decodes the start of the lines of a record: whether they are regular and W, or the number of runs
     * @param coder the decoder
     * @param nonempty whether L > 0
     * @param runs is set to the number of runs if the lines are not regular, else to 0
     * @return W, if the lines are regular and L > 0, else 0
     */
    uint64_t decode_start(range_decoder& coder, bool nonempty, uint64_t& runs)
    {
        const bool regular = !irregular.code(coder, false, last_irregular);
        last_irregular = !regular;
        runs = 0;

        if (regular && nonempty) {
            const bool is_new = new_width.code(coder, false, last_new_width);
            last_new_width = is_new;
            if (is_new) width = widths.code(coder, 0, 0) + 1;
            if (width == 0) throw invalid_file();
            return width;
        }

        if (!regular) runs = num_runs.code(coder, 0, 0) + 1;
        return 0;
    }

    /**
     * @brief decodes the next run of the lines of a record whose lines are not regular
     * @param coder the decoder
     * @return the run (length, number of lines)
     */
    std::pair<uint64_t, uint64_t> decode_run(range_decoder& coder)
    {
        const uint64_t length = run_lengths.code(coder, 0, 0);
        const uint64_t count = run_counts.code(coder, 0, 0) + 1;
        return { length, count };
    }

    /**
     * @brief decodes the number of empty lines at the end of a record (after decode_start and all runs)
     * @param coder the decoder
     */
    uint64_t decode_empty_lines(range_decoder& coder)
    {
        const uint64_t empty = empty_lines.code(coder, 0, last_empty_lines);
        last_empty_lines = empty > 0;
        return empty;
    }
};

#endif // STRING_UTILS_FASTA_LAYOUT_MODEL_HPP
