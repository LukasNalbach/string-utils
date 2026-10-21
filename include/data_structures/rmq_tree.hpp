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

#ifndef STRING_UTILS_DATA_STRUCTURES_RMQ_TREE_HPP
#define STRING_UTILS_DATA_STRUCTURES_RMQ_TREE_HPP

#include <limits>
#include <vector>

#include <data_structures/interleaved_bit_aligned_vectors.hpp>
#include <misc/utils.hpp>

/**
 * @brief min-augmented tau-ary tree over a sequence of k values: the lowest level stores the values, and
 *        each node of a higher level stores the minimum of its (at most) tau children. Supports range minimum
 *        queries in O(tau * log_tau(k)) time, and finding the previous/next value smaller than a threshold
 *        (psv/nsv) in O(tau * log_tau(k)) time. Stores about k * tau / (tau - 1) values on
 *        ceil(log_tau(k)) + 1 levels.
 * @tparam tau the arity of the tree
 */
template <uint8_t tau = 32>
class rmq_tree {
    static_assert(tau >= 2);

public:
    // ############################# CONSTANTS #############################

    // the result of a query that has no answer
    static constexpr uint64_t none = std::numeric_limits<uint64_t>::max();

protected:
    // ############################# VARIABLES #############################

    /* the first index of each level in data: [0..num_levels()] lvl_beg[l] = index in data of the first entry of
       level l; lvl_beg[num_levels()] = number of entries of all levels */
    no_init_vector<uint64_t> lvl_beg;

    // the lengths of the levels: [0..num_levels()-1] lvl_len[l] = number of entries of level l; lvl_len[0] = k
    no_init_vector<uint64_t> lvl_len;

    /* the levels, from the lowest (the values) to the root (one entry): for l in [0..num_levels()-1] and
       i in [0..lvl_len[l]-1], data[lvl_beg[l] + i] = minimum of the values [i * tau^l .. min((i + 1) * tau^l, k) - 1];
       one vector of width bits */
    interleaved_bit_aligned_vectors data;

    // ############################# INTERNAL METHODS #############################

    // returns the i-th entry of level lvl
    inline uint64_t level_value(uint8_t lvl, uint64_t i) const
    {
        return data.template get<0, uint64_t>(lvl_beg[lvl] + i);
    }

    // allocates the levels for num_values values of width bits each, without setting their values
    void allocate(uint64_t num_values, uint8_t width)
    {
        if (num_values == 0) return;
        uint64_t size = 0;

        for (uint64_t len = num_values; ; len = div_ceil<uint64_t>(len, tau)) {
            lvl_beg.emplace_back(size);
            lvl_len.emplace_back(len);
            size += len;
            if (len == 1) break;
        }

        lvl_beg.emplace_back(size);
        data = interleaved_bit_aligned_vectors({ width });
        data.resize_no_init(size);
    }

    // builds the levels above the lowest one, each from the level below it
    void build_upper_levels(uint64_t num_threads)
    {
        const bool concurrent = num_threads > 1;

        for (uint8_t lvl = 1; lvl < lvl_len.size(); lvl++) {
            #pragma omp parallel for num_threads(num_threads) if(concurrent)
            for (uint64_t i = 0; i < lvl_len[lvl]; i++) {
                uint64_t beg = i * tau;
                uint64_t end = std::min<uint64_t>(beg + tau, lvl_len[lvl - 1]);
                uint64_t min = level_value(lvl - 1, beg);

                for (uint64_t j = beg + 1; j < end; j++) {
                    min = std::min<uint64_t>(min, level_value(lvl - 1, j));
                }

                if (concurrent) data.template set_parallel<0, uint64_t>(lvl_beg[lvl] + i, min);
                else data.template set<0, uint64_t>(lvl_beg[lvl] + i, min);
            }
        }
    }

public:
    // ############################# CONSTRUCTORS #############################

    rmq_tree() = default;

    /**
     * @brief builds the tree over num_values values
     * @tparam fnc_t type of the value function
     * @param num_values number of values
     * @param width width (in bits) of the values
     * @param value function, where value(i) returns the i-th value
     * @param num_threads number of threads to use; value is called concurrently if num_threads > 1
     */
    template <typename fnc_t>
    rmq_tree(uint64_t num_values, uint8_t width, fnc_t value, uint64_t num_threads = 1)
    {
        allocate(num_values, width);
        if (num_values == 0) return;

        // with more than one thread, the entries are written with set_parallel (neighbouring entries share a word)
        const bool concurrent = num_threads > 1;

        #pragma omp parallel for num_threads(num_threads) if(concurrent)
        for (uint64_t i = 0; i < num_values; i++) {
            if (concurrent) data.template set_parallel<0, uint64_t>(i, value(i));
            else data.template set<0, uint64_t>(i, value(i));
        }

        build_upper_levels(num_threads);
    }

    /**
     * @brief builds the tree over num_values values, calling the value function sequentially from left to right
     * @tparam fnc_t type of the value function
     * @param num_values number of values
     * @param width width (in bits) of the values
     * @param value function, called exactly once as value(0), ..., value(num_values - 1), in this order
     * @param num_threads number of threads to use for building the upper levels
     * @return the tree
     */
    template <typename fnc_t>
    static rmq_tree build_sequentially(
        uint64_t num_values, uint8_t width, fnc_t value, uint64_t num_threads = 1)
    {
        rmq_tree tree;
        tree.allocate(num_values, width);

        for (uint64_t i = 0; i < num_values; i++) {
            tree.data.template set<0, uint64_t>(i, value(i));
        }

        tree.build_upper_levels(num_threads);
        return tree;
    }

    // ############################# MISC METHODS #############################

    // returns the number of levels of the tree
    inline uint8_t num_levels() const { return lvl_len.size(); }

    // returns the number of values the tree is built over
    inline uint64_t size() const { return lvl_len.empty() ? 0 : lvl_len[0]; }

    // returns whether the tree is empty
    inline bool empty() const { return size() == 0; }

    // returns the size of the tree in bytes
    uint64_t size_in_bytes() const { return data.size_in_bytes() + 2 * sizeof(uint64_t) * lvl_beg.size(); }

    // ############################# ACCESS METHODS #############################

    // returns the i-th value
    inline uint64_t operator[](uint64_t i) const { return level_value(0, i); }

    // ############################# QUERY METHODS #############################

    /**
     * @brief returns the minimum of the values at the positions i .. j
     * @param i the first position
     * @param j the last position (i <= j < size())
     * @return the minimum of the values at the positions i .. j
     */
    uint64_t range_min(uint64_t i, uint64_t j) const
    {
        uint64_t min = none;

        /* On each level, scan the values in [i..j] that are not below a node of the next level whose
           children all lie in [i..j]; then set [i..j] to the range of those nodes on the next level. */
        for (uint8_t lvl = 0; lvl < num_levels(); lvl++) {
            if (i > j) break;

            // [i..j] lies below a single node of the next level (or this is the top level): scan [i..j]
            if (i / tau == j / tau || lvl + 1 == num_levels()) {
                for (uint64_t k = i; k <= j; k++) min = std::min<uint64_t>(min, level_value(lvl, k));
                break;
            }

            // scan the values up to the end of the node containing i and from the start of the node containing j
            for (uint64_t k = i; k < (i / tau + 1) * tau; k++) min = std::min<uint64_t>(min, level_value(lvl, k));
            for (uint64_t k = (j / tau) * tau; k <= j; k++) min = std::min<uint64_t>(min, level_value(lvl, k));

            i = i / tau + 1;
            j = j / tau - 1;
        }

        return min;
    }

    /**
     * @brief returns the largest position j <= i with value(j) < threshold (psv, previous smaller value)
     * @param i a position
     * @param threshold the threshold
     * @return the largest position j <= i with value(j) < threshold, or none if there is none
     */
    uint64_t psv(uint64_t i, uint64_t threshold) const
    {
        uint8_t lvl = 0;
        uint64_t idx = i;

        while (true) {
            // scan the node and its left siblings (the children of one node of the level above)
            for (uint64_t k = idx + 1; k-- > (idx / tau) * tau;) {
                if (level_value(lvl, k) < threshold) {
                    // descend, always into the rightmost child whose subtree minimum is smaller than threshold
                    while (lvl > 0) {
                        lvl--;
                        uint64_t beg = k * tau;
                        k = std::min<uint64_t>(beg + tau, lvl_len[lvl]);
                        while (k-- > beg && level_value(lvl, k) >= threshold);
                    }

                    return k;
                }
            }

            // continue on the level above, at the left sibling of the parent of the scanned nodes
            if (lvl + 1 == num_levels() || idx < tau) return none;
            idx = idx / tau - 1;
            lvl++;
        }
    }

    /**
     * @brief returns the smallest position j >= i with value(j) < threshold (nsv, next smaller value)
     * @param i a position
     * @param threshold the threshold
     * @return the smallest position j >= i with value(j) < threshold, or none if there is none
     */
    uint64_t nsv(uint64_t i, uint64_t threshold) const
    {
        uint8_t lvl = 0;
        uint64_t idx = i;

        while (true) {
            // scan the node and its right siblings (the children of one node of the level above)
            for (uint64_t k = idx; k < std::min<uint64_t>((idx / tau + 1) * tau, lvl_len[lvl]); k++) {
                if (level_value(lvl, k) < threshold) {
                    // descend, always into the leftmost child whose subtree minimum is smaller than threshold
                    while (lvl > 0) {
                        lvl--;
                        k *= tau;
                        while (level_value(lvl, k) >= threshold) k++;
                    }

                    return k;
                }
            }

            // continue on the level above, at the right sibling of the parent of the scanned nodes
            if (lvl + 1 == num_levels()) return none;
            idx = idx / tau + 1;
            lvl++;
            if (idx >= lvl_len[lvl]) return none;
        }
    }

    // ############################# SERIALIZATION METHODS #############################

    /**
     * @brief serializes the tree to an output stream
     * @param out output stream
     */
    void serialize(std::ostream& out) const
    {
        uint8_t num_lvls = num_levels();
        out.write((char*) &num_lvls, 1);

        if (num_lvls > 0) {
            out.write((char*) &lvl_beg[0], sizeof(uint64_t) * (num_lvls + 1));
            out.write((char*) &lvl_len[0], sizeof(uint64_t) * num_lvls);
            data.serialize(out);
        }
    }

    /**
     * @brief loads the tree from an input stream
     * @param in input stream
     */
    void load(std::istream& in)
    {
        uint8_t num_lvls;
        in.read((char*) &num_lvls, 1);

        lvl_beg.clear();
        lvl_len.clear();
        data = interleaved_bit_aligned_vectors();

        if (num_lvls > 0) {
            resize_no_init(lvl_beg, num_lvls + 1);
            resize_no_init(lvl_len, num_lvls);
            in.read((char*) &lvl_beg[0], sizeof(uint64_t) * (num_lvls + 1));
            in.read((char*) &lvl_len[0], sizeof(uint64_t) * num_lvls);
            data.load(in);
        }
    }

    std::ostream& operator>>(std::ostream& os) const { serialize(os); return os; }
    std::istream& operator<<(std::istream& is) { load(is); return is; }
};

#endif // STRING_UTILS_DATA_STRUCTURES_RMQ_TREE_HPP
