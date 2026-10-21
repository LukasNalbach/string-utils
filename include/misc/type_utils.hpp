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

#ifndef STRING_UTILS_MISC_TYPE_UTILS_HPP
#define STRING_UTILS_MISC_TYPE_UTILS_HPP

#include <cstdint>
#include <type_traits>

/**
 * @brief invokes fnc with the compile-time constant std::integral_constant for each value in [start, end) stepping by inc
 * @tparam start the first value
 * @tparam end the (exclusive) end value
 * @tparam inc the increment
 * @tparam fnc_t type of the function to invoke
 * @param fnc the function to invoke for each value
 */
template <auto start, auto end, auto inc, class fnc_t>
inline constexpr void for_constexpr(fnc_t fnc)
{
    if constexpr (start < end) {
        fnc(std::integral_constant<decltype(start), start>());
        for_constexpr<start + inc, end, inc>(fnc);
    }
}

/**
 * @brief a single case of constexpr_switch: the type T is selected if the condition B holds
 * @tparam B the condition of the case
 * @tparam T the type selected if B holds
 */
template <bool B, typename T>
struct constexpr_case {
    static constexpr bool value = B;
    using type = T;
};

/**
 * @brief evaluates ::type to TrueF::type if B holds, else to FalseF::type; only the selected case is instantiated
 * @tparam B the condition
 * @tparam TrueF the case whose ::type is used if B holds
 * @tparam FalseF the case whose ::type is used otherwise
 */
template <bool B, typename TrueF, typename FalseF>
struct eval_if {
    using type = typename TrueF::type;
};

template <typename TrueF, typename FalseF>
struct eval_if<false, TrueF, FalseF> {
    using type = typename FalseF::type;
};

// alias for eval_if<B, T, F>::type
template <bool B, typename T, typename F>
using eval_if_t = typename eval_if<B, T, F>::type;

/**
 * @brief selects, at compile time, the type of the first constexpr_case in the list whose condition holds
 * @tparam Head the first case
 * @tparam Tail the remaining cases
 */
template <typename Head, typename... Tail>
struct constexpr_switch {
    using type = eval_if_t<Head::value, Head, constexpr_switch<Tail...>>;
};

template <typename T>
struct constexpr_switch<T> {
    using type = T;
};

template <bool B, typename T>
struct constexpr_switch<constexpr_case<B, T>> {
    static_assert(B, "!");
    using type = T;
};

// alias for constexpr_switch<Head, Tail...>::type
template <typename Head, typename... Tail>
using constexpr_switch_t = typename constexpr_switch<Head, Tail...>::type;

/**
 * @brief a report function called through a function pointer: it receives the values of an array one by one (or two
 *        values per position, for two arrays that share a data structure), without being a template parameter of the
 *        receiver (see make_value_report_fn)
 */
struct value_report_fn {
    void* report; // the report function
    void (*call)(void* report, int64_t value, int64_t second); // calls the report function

    inline void operator()(int64_t value, int64_t second = 0) const { call(report, value, second); }
};

/**
 * @brief returns a value_report_fn that calls report(value, second)
 * @tparam report_t type of the report function
 * @param report the report function; it has to live as long as the returned value_report_fn is used
 */
template <typename report_t>
inline value_report_fn make_value_report_fn(report_t& report)
{
    return { &report, [](void* target, int64_t value, int64_t second) {
        (*static_cast<report_t*>(target))(value, second);
    } };
}

#endif // STRING_UTILS_MISC_TYPE_UTILS_HPP
