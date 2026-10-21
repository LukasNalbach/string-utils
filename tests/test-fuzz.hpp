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

#ifndef STRING_UTILS_TESTS_TEST_FUZZ_HPP
#define STRING_UTILS_TESTS_TEST_FUZZ_HPP

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <omp.h>

#include <misc/parse.hpp>
#include <misc/platform.hpp>

// default number of random inputs a test validates (see fuzz_iterations)
inline constexpr uint64_t default_fuzz_iterations = 1000;

// default maximum time in seconds one functionality is fuzzed (see fuzz_seconds)
inline constexpr double default_fuzz_seconds = 10.0;

/**
 * @brief returns the maximum number of random inputs a test validates (see run_fuzz), overridable via
 *        <prefix>_TEST_ITERATIONS
 * @param default_iterations the default iteration count if the environment variable is unset/invalid
 */
inline uint64_t fuzz_iterations(uint64_t default_iterations = default_fuzz_iterations)
{
    const uint64_t value = env_uint(STRING_UTILS_ENV_PREFIX "_TEST_ITERATIONS", 0);
    return value > 0 ? value : default_iterations;
}

/**
 * @brief returns the maximum time in seconds one functionality is fuzzed (see run_fuzz), overridable via
 *        <prefix>_TEST_SECONDS (a point is the decimal separator under every locale)
 * @param default_seconds the default time if the environment variable is unset/invalid
 */
inline double fuzz_seconds(double default_seconds = default_fuzz_seconds)
{
    const double value = env_double(STRING_UTILS_ENV_PREFIX "_TEST_SECONDS", 0);
    return value > 0 ? value : default_seconds;
}

/**
 * @brief returns the seed for the random number generators of a test; it is drawn once per process and
 *        printed at the first call. Setting <prefix>_TEST_SEED to a printed seed replays the same inputs in
 *        the same order.
 */
inline uint64_t test_seed()
{
    static const uint64_t seed = [] {
        uint64_t value;

        if (const char* env = std::getenv(STRING_UTILS_ENV_PREFIX "_TEST_SEED")) {
            value = std::strtoull(env, nullptr, 10);
        } else {
            value = (uint64_t(std::random_device {}()) << 32) | std::random_device {}();
        }

        std::cout << "seed: " << value << " (replay with " STRING_UTILS_ENV_PREFIX "_TEST_SEED=" << value << ")"
                  << std::endl;
        return value;
    }();

    return seed;
}

/**
 * @brief a named check of one functionality (e.g. count, locate, random access, serialize) of a data structure;
 *        run(iteration) builds a random data structure and checks this functionality
 *
 * parallel = true: run_fuzz calls run from several threads at the same time (run must then be thread-safe and use
 * a thread-local random number generator); parallel = false: run_fuzz calls run from one thread at a time.
 */
struct fuzz_functionality {
    std::string name;
    std::function<void(uint64_t)> run;
    bool parallel = false;
};

/**
 * @brief fuzzes a data structure by running each of its functionalities on random inputs and prints, per
 *        functionality, a progress bar with the number of validated inputs and the elapsed time to stderr
 *
 * Each functionality stops after its share of the iterations or after fuzz_seconds() seconds; at least one
 * input is validated. If input_size is given, the maximum input length is reduced during the run (down to
 * min_input_size), such that the iterations fit into the time. If <prefix>_TEST_SEED is set, neither the time
 * limit nor the reduction of the input length is applied.
 *
 * @param data_structure the name of the data structure (shown in the progress bar)
 * @param functionalities the functionalities to fuzz
 * @param iterations the number of inputs to validate over the whole test, split evenly over the
 *        functionalities
 * @param input_size pointer to the maximum input length used by the caller, which is adjusted during the
 *        run and restored at the end; nullptr: the input length is not adjusted
 * @param min_input_size minimum value input_size is reduced to; 0: max(64, *input_size / 32)
 */
inline void run_fuzz(
    const std::string& data_structure, const std::vector<fuzz_functionality>& functionalities,
    uint64_t iterations = fuzz_iterations(),
    uint64_t* input_size = nullptr, uint64_t min_input_size = 0)
{
    using clock = std::chrono::steady_clock;
    const bool is_terminal = platform::is_terminal(stderr);
    const char* green_color = is_terminal ? "\033[0;32m" : "";
    const char* reset_color = is_terminal ? "\033[m" : "";

    // the input size is reduced down to max(min_reduced_input_size, input size / reduced_input_size_divisor) by default
    constexpr uint64_t min_reduced_input_size = 64;
    constexpr uint64_t reduced_input_size_divisor = 32;
    // a window has target / window_divisor inputs, at least min_window_length and at most max_window_length
    constexpr uint64_t window_divisor = 8;
    constexpr uint64_t min_window_length = 32;
    constexpr uint64_t max_window_length = 128;
    // a batch takes about 1 / batches_per_time_limit of the time limit and has at most max_batch_size inputs
    constexpr double batches_per_time_limit = 100.0;
    constexpr uint64_t max_batch_size = 4096;
    // the size is reduced if the time per input exceeds the remaining time per remaining input times max_overtime
    constexpr double max_overtime = 1.25;
    // the size is restored if a reduction leaves more than max_time_ratio_after_reduction of the time per input
    constexpr double max_time_ratio_after_reduction = 0.9;
    // a reduction multiplies the size by (remaining time per input / time per input)^reduction_exponent, clamped to
    // [min_reduction_factor, max_reduction_factor]
    constexpr double reduction_exponent = 0.75;
    constexpr double min_reduction_factor = 0.25;
    constexpr double max_reduction_factor = 0.9;
    // smallest time per input in seconds used to compute the batch size
    constexpr double min_time_per_input = 1e-9;

    const uint64_t total_iterations = std::max<uint64_t>(1, iterations);
    const uint64_t num_functionalities = std::max<uint64_t>(1, functionalities.size());
    const double max_seconds = fuzz_seconds();
    // true <=> a seed is given; then the time limit and the adjustment of the input size are disabled
    const bool is_replay = std::getenv(STRING_UTILS_ENV_PREFIX "_TEST_SEED") != nullptr;

    auto seconds_since = [](clock::time_point t) {
        return std::chrono::duration<double>(clock::now() - t).count();
    };

    // the input size is reduced down to min_size
    const uint64_t original_size = input_size != nullptr ? *input_size : 0;
    const uint64_t min_size = min_input_size != 0 ? min_input_size
        : std::max<uint64_t>(min_reduced_input_size, original_size / reduced_input_size_divisor);

    int32_t last_printed_percent = -1;
    uint64_t cur_target = 1; // number of inputs of the current functionality
    uint64_t printed_input_size = 0; // the adjusted input size (0 if it has not been adjusted), shown next to the count

    // prints the progress bar of the functionality label: done of cur_target inputs after elapsed seconds
    auto print_progress_line = [&](const std::string& label, uint64_t done, double elapsed) {
        constexpr int32_t bar_width = 10; // number of columns inside the brackets
        // the bar shows the larger of the fractions of the inputs done and of the time elapsed
        const double fraction = std::min(1.0,
            std::max((double) done / (double) cur_target, elapsed / max_seconds));
        int32_t filled = (int32_t) (fraction * bar_width + 0.5);
        if (filled > bar_width) filled = bar_width;
        std::string bar(filled, '=');
        bar.resize(bar_width, ' ');
        // on a terminal, overwrite the same line (\r); otherwise print a new line
        std::fprintf(stderr, "%s%s[%s]%s %-28s %7" PRIu64 "/%-7" PRIu64 " %5.1fs",
            is_terminal ? "\r" : "\n", green_color, bar.c_str(), reset_color, (data_structure + "/" + label).c_str(),
            (uint64_t) done, (uint64_t) cur_target, elapsed);
        if (printed_input_size != 0) std::fprintf(stderr, "  n<=%" PRIu64, printed_input_size);
        std::fprintf(stderr, "   ");
        std::fflush(stderr);
    };

    for (uint64_t f = 0; f < functionalities.size(); f++) {
        const fuzz_functionality& functionality = functionalities[f];
        // number of inputs of this functionality
        const uint64_t target = std::max<uint64_t>(1,
            total_iterations / num_functionalities
                + (f < total_iterations % num_functionalities ? 1 : 0));
        const auto functionality_start = clock::now();

        cur_target = target;
        if (input_size != nullptr) *input_size = original_size;

        uint64_t done = 0; // number of validated inputs

        // prints the progress line if the percentage of done inputs changed (without a terminal: every 10 %)
        auto print_progress = [&](double elapsed) {
            const int32_t percent = (int32_t) (100.0 * (double) done / (double) target + 0.5);

            if (percent != last_printed_percent && (is_terminal || percent % 10 == 0)) {
                last_printed_percent = percent;
                print_progress_line(functionality.name, done, elapsed);
            }
        };

        // runs batch iterations, sequentially or (for a parallel functionality) distributed over the threads
        auto run_batch = [&](uint64_t batch) {
            if (functionality.parallel) {
                std::atomic<uint64_t> finished { 0 };
                const uint64_t first = done;

                #pragma omp parallel for schedule(dynamic)
                for (int64_t iteration = 0; iteration < (int64_t) batch; iteration++) {
                    functionality.run(first + (uint64_t) iteration);
                    const uint64_t reached = first + finished.fetch_add(1) + 1;

                    if (omp_get_thread_num() == 0) {
                        done = reached;
                        print_progress(seconds_since(functionality_start));
                    }
                }

                done = first + batch;
            } else {
                for (uint64_t iteration = 0; iteration < batch; iteration++) {
                    functionality.run(done);
                    done++;
                    print_progress(seconds_since(functionality_start));
                }
            }
        };

        /* The input size is adjusted after each window of inputs, based on the average time per input in
           the window. The adjustment stops if the last reduction decreased the time per input by less than
           1 - max_time_ratio_after_reduction (then the previous size is restored). */
        const uint64_t window_length =
            std::clamp<uint64_t>(target / window_divisor, min_window_length, max_window_length);
        bool is_adapting_size = input_size != nullptr && !is_replay;

        uint64_t window_inputs = 0; // number of inputs in the current window
        double window_started = 0; // time in seconds at which the current window started
        double time_per_input_before_change = 0; // average time per input in seconds before the last change of the size
        uint64_t size_before_change = 0; // input size before the last change (0 if there was none)

        while (done == 0 || (done < target && (is_replay || seconds_since(functionality_start) < max_seconds))) {
            const double elapsed_before_batch = seconds_since(functionality_start);
            uint64_t batch = 1;

            if (done > 0 && elapsed_before_batch > 0) {
                // choose the batch size such that a batch takes about 1 / batches_per_time_limit of the time limit
                const double per_input = elapsed_before_batch / (double) done;
                batch = std::clamp<uint64_t>((uint64_t) ((max_seconds / batches_per_time_limit)
                    / std::max(per_input, min_time_per_input)), 1, max_batch_size);
            }

            batch = std::min(batch, target - done);
            run_batch(std::max<uint64_t>(1, batch));

            if (!is_adapting_size || done >= target) continue;

            window_inputs += std::max<uint64_t>(1, batch);
            const double elapsed = seconds_since(functionality_start);
            if (window_inputs < window_length) continue;

            const double time_per_input = (elapsed - window_started) / (double) window_inputs;
            // remaining time per remaining input in seconds
            const double max_time_per_input = std::max(0.0, max_seconds - elapsed) / (double) (target - done);

            window_inputs = 0;
            window_started = elapsed;

            if (size_before_change != 0
                && time_per_input > time_per_input_before_change * max_time_ratio_after_reduction) {
                // the last reduction decreased the time per input too little: restore the size and stop
                *input_size = size_before_change;
                printed_input_size = *input_size;
                is_adapting_size = false;
                continue;
            }

            if (time_per_input <= max_time_per_input * max_overtime || *input_size <= min_size) {
                size_before_change = 0;
                continue;
            }

            const double factor = std::clamp(max_time_per_input > 0
                ? std::pow(max_time_per_input / time_per_input, reduction_exponent) : min_reduction_factor,
                min_reduction_factor, max_reduction_factor);
            time_per_input_before_change = time_per_input;
            size_before_change = *input_size;
            *input_size = std::max<uint64_t>(min_size, (uint64_t) ((double) *input_size * factor));
            printed_input_size = *input_size;
        }

        // leave the line of the completed functionality (with its input count and time) on the terminal
        print_progress_line(functionality.name, done, seconds_since(functionality_start));
        std::fputc('\n', stderr);
        last_printed_percent = -1;
        printed_input_size = 0;
    }

    if (input_size != nullptr) *input_size = original_size;
}

#endif // STRING_UTILS_TESTS_TEST_FUZZ_HPP
