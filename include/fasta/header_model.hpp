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
 * @file header_model.hpp
 * @brief the context models of the header lines of a FASTA file: each header is split into tokens and coded token
 *        by token relative to the tokens of the previous header
 */

#ifndef STRING_UTILS_FASTA_HEADER_MODEL_HPP
#define STRING_UTILS_FASTA_HEADER_MODEL_HPP

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <coding/byte_codes.hpp>
#include <coding/integer_model.hpp>
#include <coding/mixer.hpp>
#include <coding/range_coder.hpp>

/**
 * @brief codes the headers of a FASTA file one after another. Token j of a header is coded with an operation (see
 *        token_op) in the contexts of the operation at position j of the previous header, the type of its token j
 *        and the operation of token j - 1. A difference is coded as its sign and its absolute value minus one, in the
 *        context of j. A literal token is coded as its type, then as a single other byte, or as its digits or letters
 *        followed by an end symbol, each with three mixed models (contexts: the previous two symbols of the token; the
 *        symbol at the same offset of token j of the previous header and the previous symbol; j and the previous
 *        symbol).
 */
class fasta_header_model {
private:
    // the kinds of tokens: a maximal run of digits, a maximal run of letters (A-Z, a-z), or a single other byte
    enum token_type : uint8_t { token_digits = 0, token_letters = 1, token_other = 2 };

    // number of token types
    static constexpr uint64_t num_token_types = 3;

    /* how a token is coded: op_match = equal to the token at the same position of the previous header, op_delta = a
       number that differs from the number at the same position of the previous header by a coded difference,
       op_literal = coded symbol by symbol, op_end = the header has no more tokens */
    enum token_op : uint8_t { op_match = 0, op_delta = 1, op_literal = 2, op_end = 3 };

    // number of token operations
    static constexpr uint64_t num_token_ops = 4;

    // number of token positions with their own contexts; the positions from this one on share the contexts of the last
    // one
    static constexpr uint64_t header_token_positions = 64;

    // number of decimal digits of the numbers that the operation op_delta codes (all of them fit into an int64_t)
    static constexpr uint64_t max_delta_digits = std::numeric_limits<int64_t>::digits10;

    // number of decimal digits
    static constexpr uint64_t num_decimal_digits = 10;

    // number of letters (A-Z and a-z)
    static constexpr uint64_t num_letters = 2 * ('Z' - 'A' + 1);

    /**
     * @brief a token of a header: header[begin..end-1]
     */
    struct header_token {
        uint64_t begin = 0; // the first byte
        uint64_t end = 0; // the byte after the last one
        token_type type = token_other; // its type
    };

    // returns the type of the token that contains the byte c
    static token_type type_of(uint8_t c)
    {
        if (c >= '0' && c <= '9') return token_digits;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return token_letters;
        return token_other;
    }

    /**
     * @brief splits a header into its tokens
     * @param header the header
     * @param tokens is set to the tokens
     */
    static void tokenize(const std::string& header, std::vector<header_token>& tokens)
    {
        tokens.clear();

        for (uint64_t i = 0; i < header.size();) {
            const token_type type = type_of(uint8_t(header[i]));
            uint64_t j = i + 1;
            if (type != token_other) while (j < header.size() && type_of(uint8_t(header[j])) == type) j++;
            tokens.push_back({ i, j, type });
            i = j;
        }
    }

    // symbol of a digit or a letter token that ends it
    static constexpr uint64_t digit_end = num_decimal_digits;
    static constexpr uint64_t letter_end = num_letters;
    // context values of "no symbol"
    static constexpr uint64_t no_digit = digit_end + 1;
    static constexpr uint64_t no_letter = letter_end + 1;
    static constexpr uint64_t no_byte = num_byte_values;
    static constexpr uint64_t no_op = num_token_ops;
    static constexpr uint64_t no_type = num_token_types;

    mixed_trees<3> ops; // the models of the operations
    decision_model delta_negative; // the models of the signs of the deltas
    integer_model delta_magnitude; // the models of the absolute values of the deltas minus one
    bit_trees types; // the models of the types of literal tokens
    mixed_trees<3> digit_symbols; // the models of the digits of literal tokens
    mixed_trees<3> letter_symbols; // the models of the letters of literal tokens
    mixed_trees<3> other_bytes; // the models of the bytes of literal tokens of type token_other

    /**
     * @brief a header with its tokens and their operations
     */
    struct reference_header {
        std::string text; // the header
        std::vector<header_token> tokens; // its tokens
        std::vector<uint8_t> ops; // the operations of its tokens, followed by op_end
    };

    reference_header previous; // the previous header
    std::vector<header_token> cur_tokens; // the tokens of the current header
    std::vector<uint8_t> cur_ops; // the operations of its tokens, followed by op_end

    // returns the context position of token j
    static uint64_t position(uint64_t j) { return std::min(j, header_token_positions - 1); }

    // returns the symbol of a digit or letter c
    static uint64_t symbol_of(uint8_t c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'Z') return c - 'A';
        return (c - 'a') + num_letters / 2;
    }

    // returns the byte of a symbol of a token of the type
    static uint8_t byte_of(uint64_t symbol, token_type type)
    {
        if (type == token_digits) return uint8_t('0' + symbol);
        return uint8_t(symbol < num_letters / 2 ? 'A' + symbol : 'a' + (symbol - num_letters / 2));
    }

    // returns the operation at position j of the previous header (no_op if it has fewer tokens)
    uint64_t prev_op(uint64_t j) const { return j < previous.ops.size() ? previous.ops[j] : no_op; }

    // returns the type of token j of the previous header (no_type if it has fewer tokens)
    uint64_t prev_type(uint64_t j) const { return j < previous.tokens.size() ? uint64_t(previous.tokens[j].type) : no_type; }

    // returns the contexts of the operation of token j, whose predecessor in the current header has the operation op
    std::array<uint64_t, 3> op_contexts(uint64_t j, uint64_t op) const
    {
        return { position(j) * (no_op + 1) + prev_op(j), op * (no_type + 1) + prev_type(j),
            position(j) * (no_op + 1) + op };
    }

    /**
     * @brief returns the value of a token of digits with at most max_delta_digits digits
     * @param header the header
     * @param token the token
     */
    static int64_t value_of(const std::string& header, const header_token& token)
    {
        int64_t value = 0;
        for (uint64_t i = token.begin; i < token.end; i++) value = value * int64_t(num_decimal_digits) + (header[i] - '0');
        return value;
    }

    /**
     * @brief codes the symbols of a literal token of digits or letters, followed by the end symbol
     * @param coder a range_encoder or a range_decoder
     * @param type the type of the token
     * @param j the position of the token
     * @param header the current header; the encoder reads the token from it, the decoder appends it to it
     * @param token the token (the encoder only)
     */
    template <typename coder_t>
    void code_symbols(coder_t& coder, token_type type, uint64_t j, std::string& header, const header_token& token)
    {
        constexpr bool encoding = std::is_same_v<coder_t, range_encoder>;
        const bool is_digits = type == token_digits;
        const uint64_t end_symbol = is_digits ? digit_end : letter_end;
        const uint64_t none = is_digits ? no_digit : no_letter;
        mixed_trees<3>& models = is_digits ? digit_symbols : letter_symbols;
        // the token at position j of the previous header, if it has the same type
        const bool has_reference = j < previous.tokens.size() && previous.tokens[j].type == type;
        uint64_t last = none, before_last = none; // the previous two symbols of the token

        for (uint64_t offset = 0;; offset++) {
            uint64_t predicted = none;

            if (has_reference) {
                const uint64_t i = previous.tokens[j].begin + offset;
                predicted = i < previous.tokens[j].end ? symbol_of(uint8_t(previous.text[i])) : end_symbol;
            }

            uint64_t symbol = end_symbol;
            if constexpr (encoding) {
                if (token.begin + offset < token.end) symbol = symbol_of(uint8_t(header[token.begin + offset]));
            }

            symbol = models.code(coder, symbol, { before_last * (none + 1) + last, predicted * (none + 1) + last,
                position(j) * (none + 1) + last });
            if (symbol > end_symbol || (symbol == end_symbol && offset == 0)) throw invalid_file();
            if (symbol == end_symbol) return;
            if constexpr (!encoding) header.push_back(char(byte_of(symbol, type)));
            before_last = last;
            last = symbol;
        }
    }

    /**
     * @brief codes the tokens of a header after they have been chosen (encoder) or decodes them (decoder)
     * @param coder a range_encoder or a range_decoder
     * @param header the header; the decoder appends the decoded bytes to it
     */
    template <typename coder_t>
    void code_tokens(coder_t& coder, std::string& header)
    {
        constexpr bool encoding = std::is_same_v<coder_t, range_encoder>;
        uint64_t last_op = no_op;
        uint64_t last_byte = no_byte; // the last byte of the header

        for (uint64_t j = 0;; j++) {
            uint64_t op = encoding ? cur_ops[j] : 0;
            op = ops.code(coder, op, op_contexts(j, last_op));
            if constexpr (!encoding) cur_ops.push_back(uint8_t(op));
            if (op == op_end) break;
            if (op != op_literal && j >= previous.tokens.size()) throw invalid_file();

            if (op == op_match) {
                if constexpr (!encoding) {
                    header.append(previous.text, previous.tokens[j].begin, previous.tokens[j].end - previous.tokens[j].begin);
                }
            } else if (op == op_delta) {
                const header_token& reference = previous.tokens[j];
                const uint64_t width = reference.end - reference.begin;
                if (reference.type != token_digits || width > max_delta_digits) throw invalid_file();
                const int64_t reference_value = value_of(previous.text, reference);
                int64_t difference = 0;
                if constexpr (encoding) difference = value_of(header, cur_tokens[j]) - reference_value;
                const bool negative = delta_negative.code(coder, difference < 0, position(j));
                const uint64_t magnitude =
                    delta_magnitude.code(coder, uint64_t(negative ? -difference : difference) - 1, position(j)) + 1;

                if constexpr (!encoding) {
                    if (magnitude > (negative ? uint64_t(reference_value)
                        : uint64_t(std::numeric_limits<int64_t>::max() - reference_value))) {
                        throw invalid_file();
                    }

                    const int64_t value = reference_value + (negative ? -int64_t(magnitude) : int64_t(magnitude));
                    std::string number = std::to_string(value);
                    if (number.size() < width) number.insert(0, width - number.size(), '0');
                    header.append(number);
                }
            } else {
                const uint64_t last_type = last_byte == no_byte ? no_type : uint64_t(type_of(uint8_t(last_byte)));
                uint64_t type = encoding ? cur_tokens[j].type : 0;
                type = types.code(coder, type, std::bit_width(no_type - 1), last_type * (no_type + 1) + prev_type(j));
                if (type >= num_token_types) throw invalid_file();

                if (type == token_other) {
                    const uint64_t reference = j < previous.tokens.size() && previous.tokens[j].type == token_other
                        ? uint8_t(previous.text[previous.tokens[j].begin]) : no_byte;
                    uint64_t c = encoding ? uint8_t(header[cur_tokens[j].begin]) : 0;
                    c = other_bytes.code(coder, c, { position(j), reference, last_byte });
                    if (type_of(uint8_t(c)) != token_other) throw invalid_file();
                    if constexpr (!encoding) header.push_back(char(c));
                } else {
                    code_symbols(coder, token_type(type), j, header, encoding ? cur_tokens[j] : header_token {});
                }
            }

            last_op = op;
            if (!header.empty()) last_byte = uint8_t(encoding ? header[cur_tokens[j].end - 1] : header.back());
        }
    }

    /**
     * @brief chooses the operations of the tokens of the current header relative to the previous header
     * @param header the current header
     */
    void choose_ops(const std::string& header)
    {
        cur_ops.clear();

        for (uint64_t j = 0; j < cur_tokens.size(); j++) {
            const header_token& token = cur_tokens[j];
            const uint64_t width = token.end - token.begin;
            uint8_t op = op_literal;

            if (j < previous.tokens.size()) {
                const header_token& reference = previous.tokens[j];
                const uint64_t reference_width = reference.end - reference.begin;

                if (header.compare(token.begin, width, previous.text, reference.begin, reference_width) == 0) {
                    op = op_match;
                } else if (token.type == token_digits && reference.type == token_digits && width <= max_delta_digits &&
                    reference_width <= max_delta_digits && width >= reference_width &&
                    (width == reference_width || header[token.begin] != '0')) {
                    op = op_delta;
                }
            }

            cur_ops.push_back(op);
        }

        cur_ops.push_back(op_end);
    }

    // makes the current header the previous one
    void advance(std::string& header)
    {
        std::swap(previous.text, header);
        tokenize(previous.text, previous.tokens);
        std::swap(previous.ops, cur_ops);
        cur_ops.clear();
    }

public:
    // creates the models
    fasta_header_model()
        : ops(std::bit_width(num_token_ops - 1), { header_token_positions * (no_op + 1), (no_op + 1) * (no_type + 1),
              header_token_positions * (no_op + 1) }),
          delta_negative(header_token_positions),
          delta_magnitude(header_token_positions),
          types(std::bit_width(no_type - 1), (no_type + 1) * (no_type + 1)),
          digit_symbols(std::bit_width(digit_end), { (no_digit + 1) * (no_digit + 1), (no_digit + 1) * (no_digit + 1),
              header_token_positions * (no_digit + 1) }),
          letter_symbols(std::bit_width(letter_end), { (no_letter + 1) * (no_letter + 1),
              (no_letter + 1) * (no_letter + 1), header_token_positions * (no_letter + 1) }),
          other_bytes(std::numeric_limits<uint8_t>::digits, { header_token_positions, no_byte + 1, no_byte + 1 }) { }

    /**
     * @brief encodes a header
     * @param coder the encoder
     * @param header the header (without '>' and without the line end)
     */
    void encode(range_encoder& coder, std::string header)
    {
        tokenize(header, cur_tokens);
        choose_ops(header);
        code_tokens(coder, header);
        advance(header);
    }

    /**
     * @brief decodes a header
     * @param coder the decoder
     * @return the header
     */
    std::string decode(range_decoder& coder)
    {
        std::string header;
        cur_ops.clear();
        code_tokens(coder, header);
        std::string result = header;
        advance(header);
        return result;
    }
};

#endif // STRING_UTILS_FASTA_HEADER_MODEL_HPP
