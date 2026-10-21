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

#ifndef STRING_UTILS_MISC_ALIGNED_TABLE_HPP
#define STRING_UTILS_MISC_ALIGNED_TABLE_HPP

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/**
 * @brief collects labeled rows of values and prints them with a header of the column indexes: each label is
 *        left-aligned to the longest label, each column is right-aligned to its widest cell, and cells are separated
 *        by one space
 */
class aligned_table {
    // the labels of the rows: [0.._labels.size()-1] _labels[r] is the label of row r
    std::vector<std::string> _labels;
    // the cells of the rows as text: [0.._labels.size()-1] _rows[r][i] is cell i of row r
    std::vector<std::vector<std::string>> _rows;

  public:
    /**
     * @brief adds a row labeled label whose i-th cell (for i in [0, count)) is the value cell(i)
     * @tparam cell_fnc_t type of the cell-value function
     * @param label the row label
     * @param count the number of cells in the row
     * @param cell function mapping a cell index to its (streamable) value
     */
    template <typename cell_fnc_t>
    void add_row(const std::string& label, uint64_t count, cell_fnc_t cell)
    {
        _labels.emplace_back(label);
        std::vector<std::string>& row = _rows.emplace_back();
        row.reserve(count);

        for (uint64_t i = 0; i < count; i++) {
            std::ostringstream s;
            s << cell(i);
            row.emplace_back(s.str());
        }
    }

    /**
     * @brief prints the header of the column indexes (0, 1, ...) and the rows to out
     * @param out the output stream to print to
     */
    void print(std::ostream& out = std::cout) const
    {
        uint64_t label_width = 0;
        uint64_t num_columns = 0;

        for (const std::string& label : _labels)
            label_width = std::max<uint64_t>(label_width, label.size());

        for (const std::vector<std::string>& row : _rows)
            num_columns = std::max<uint64_t>(num_columns, row.size());

        // the width of column i: the maximum length of i and of the cells in column i
        std::vector<uint64_t> column_width(num_columns, 0);

        for (uint64_t i = 0; i < num_columns; i++)
            column_width[i] = std::to_string(i).size();

        for (const std::vector<std::string>& row : _rows)
            for (uint64_t i = 0; i < row.size(); i++)
                column_width[i] = std::max<uint64_t>(column_width[i], row[i].size());

        // header row: spaces in place of a label, then the column indexes
        out << std::string(label_width, ' ');

        for (uint64_t i = 0; i < num_columns; i++) {
            std::string index = std::to_string(i);
            out << ' ' << std::string(column_width[i] - index.size(), ' ') << index;
        }

        out << std::endl;

        for (uint64_t r = 0; r < _rows.size(); r++) {
            out << _labels[r] << std::string(label_width - _labels[r].size(), ' ');

            for (uint64_t i = 0; i < _rows[r].size(); i++)
                out << ' ' << std::string(column_width[i] - _rows[r][i].size(), ' ') << _rows[r][i];

            out << std::endl;
        }
    }
};

/**
 * @brief prints an array with a label and a header of the indexes to std::cout (see aligned_table)
 * @tparam container_t container type (must support empty(), size() and operator[])
 * @param label the label of the array
 * @param container the array to print
 */
template <typename container_t>
inline void log_indexed(const std::string& label, const container_t& container)
{
    if (container.empty()) return;
    aligned_table table;
    table.add_row(label, container.size(), [&](uint64_t i) { return container[i]; });
    table.print();
}

#endif // STRING_UTILS_MISC_ALIGNED_TABLE_HPP
