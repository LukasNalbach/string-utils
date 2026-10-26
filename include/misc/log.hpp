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

#ifndef STRING_UTILS_MISC_LOG_HPP
#define STRING_UTILS_MISC_LOG_HPP

#include <algorithm>
#include <numeric>
#include <array>
#include <cstdint>
#include <atomic>
#include <bit>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>
#include <malloc_count.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <vector>

#include <misc/platform.hpp>

// the stream the log functions write to: std::cout, or std::cerr if std::cout carries the output of a tool
inline std::ostream* log_stream = &std::cout;

// returns the stream the log functions write to
inline std::ostream& log_out() { return *log_stream; }

// returns the current time point of the steady clock
static std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

/**
 * @brief formats a duration in nanoseconds with an appropriate unit (ns, us, ms, s, min or h)
 * @param ns a duration in nanoseconds
 * @return the formatted duration; from one minute on as minutes:seconds (e.g. "7:03 min") or
 *         hours:minutes (e.g. "2:41 h")
 */
static std::string format_time(uint64_t ns)
{
    std::string time_str;

    if (ns >= 3600000000000) {
        uint64_t minutes = ns / 60000000000;
        std::string min_str = std::to_string(minutes % 60);
        time_str = std::to_string(minutes / 60) + ":" + std::string(2 - min_str.size(), '0') + min_str + " h";
    } else if (ns >= 60000000000) {
        uint64_t seconds = ns / 1000000000;
        std::string sec_str = std::to_string(seconds % 60);
        time_str = std::to_string(seconds / 60) + ":" + std::string(2 - sec_str.size(), '0') + sec_str + " min";
    } else if (ns >= 10000000000) {
        time_str = std::to_string(ns / 1000000000) + " s";
    } else if (ns >= 10000000) {
        time_str = std::to_string(ns / 1000000) + " ms";
    } else if (ns >= 10000) {
        time_str = std::to_string(ns / 1000) + " us";
    } else {
        time_str = std::to_string(ns) + " ns";
    }

    return time_str;
}

/**
 * @brief formats an estimated duration in nanoseconds, rounded down to seconds (see log_progress)
 * @param ns the estimated duration in nanoseconds
 * @return the formatted duration ("< 1 s" below one second)
 */
inline std::string format_eta(uint64_t ns)
{
    if (ns < 1000000000) return "< 1 s";
    if (ns < 60000000000) return std::to_string(ns / 1000000000) + " s";
    return format_time(ns);
}

/**
 * @brief computes an index-construction throughput in MB/s (megabytes = 10^6 bytes, matching format_size)
 * @param n_bytes the number of input bytes processed (the text length)
 * @param time_ns the construction duration in nanoseconds
 * @return the throughput in MB/s (0 if time_ns is 0)
 */
inline double throughput_in_mb_per_s(uint64_t n_bytes, uint64_t time_ns)
{
    return time_ns == 0 ? 0.0 : (n_bytes * 1000.0) / time_ns; // (n_bytes / 1e6) / (time_ns / 1e9)
}

/**
 * @brief formats an index-construction throughput in MB/s (megabytes = 10^6 bytes, matching format_size)
 * @param n_bytes the number of input bytes processed (the text length)
 * @param time_ns the construction duration in nanoseconds
 * @return the formatted throughput, e.g. "123.45 MB/s"
 */
inline std::string format_throughput(uint64_t n_bytes, uint64_t time_ns)
{
    std::ostringstream str;
    str << std::fixed << std::setprecision(2) << throughput_in_mb_per_s(n_bytes, time_ns) << " MB/s";
    return str.str();
}

// the maximum number of decimals of a ratio (see format_ratio)
inline constexpr int ratio_decimals = 2;

/**
 * @brief formats a ratio with at most ratio_decimals decimals, without trailing zeros
 * @param value the ratio
 * @return the formatted ratio, e.g. 28.72, 0.5 or 3
 */
inline std::string format_ratio(double value)
{
    std::ostringstream str;
    str << std::fixed << std::setprecision(ratio_decimals) << value;
    std::string formatted = str.str();
    if (formatted.find('.') == std::string::npos) return formatted;
    while (formatted.back() == '0') formatted.pop_back();
    if (formatted.back() == '.') formatted.pop_back();
    return formatted;
}

/**
 * @brief formats the share of a part of a whole in percent
 * @param part the part
 * @param whole the whole
 * @return the share with at most ratio_decimals decimals, e.g. "3.5 %"
 */
inline std::string format_percentage(uint64_t part, uint64_t whole)
{
    return format_ratio(whole == 0 ? 0.0 : 100.0 * double(part) / double(whole)) + " %";
}

/**
 * @brief formats a number with a separator every three digits
 * @param value the number
 * @return the formatted number, e.g. 1,000,000
 */
inline std::string format_number(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);

    for (uint64_t i = 0; i < digits.size(); i++) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }

    return out;
}

/**
 * @brief formats a number of threads
 * @param p the number of threads
 * @return "1 thread" or "<p> threads"
 */
inline std::string format_threads(uint64_t p) { return p == 1 ? "1 thread" : format_number(p) + " threads"; }

/**
 * @brief formats a size in bytes with an appropriate unit (B, KB, MB or GB)
 * @param B a size in bytes
 * @return the formatted size (sizes in GB with two decimals)
 */
inline std::string format_size(uint64_t B)
{
    std::string size_str;

    if (B > 10000000000) {
        std::ostringstream gb;
        gb << std::fixed << std::setprecision(2) << (B / 1000000000.0) << " GB";
        size_str = gb.str();
    } else if (B > 10000000) {
        size_str = std::to_string(B / 1000000) + " MB";
    } else if (B > 10000) {
        size_str = std::to_string(B / 1000) + " KB";
    } else {
        size_str = std::to_string(B) + " B";
    }

    return size_str;
}

// size in bytes of the data structure whose components are being listed (0 if no listing is in progress);
// set by begin_listing and reset to 0 by end_listing (see format_share)
inline uint64_t listing_total = 0;
// sum of the sizes in bytes of the components listed with log_component at depth 0 since begin_listing
inline uint64_t listing_listed = 0;
// number of levels by which log_component indents the components in addition to their depth (e.g. 1 for the
// components listed under a heading)
inline uint64_t listing_indent = 0;

/**
 * @brief formats a size and, if listing_total is set, its share of listing_total
 * @param B a size in bytes
 * @return the formatted size, followed by its share in percent, e.g. "12 MB (3.4 %)"
 */
inline std::string format_share(uint64_t B)
{
    std::string size_str = format_size(B);
    if (listing_total == 0) return size_str;

    std::ostringstream share;
    share << size_str << " (" << std::fixed << std::setprecision(1)
          << (100.0 * double(B) / double(listing_total)) << " %)";
    return share.str();
}

/**
 * @brief starts a listing of the sizes of the components of a data structure (see log_component), unless a listing is
 *        in progress
 * @param total the size of the data structure in bytes
 * @return whether a listing was started (then the caller ends it with end_listing)
 */
inline bool begin_listing(uint64_t total)
{
    if (listing_total != 0) return false;
    listing_total = std::max<uint64_t>(total, 1);
    listing_listed = 0;
    return true;
}

/**
 * @brief logs the size of a component of the data structure whose sizes are being listed, with its share of
 *        listing_total: "<name>: <size> (<share> %)", indented by two spaces per depth (plus listing_indent)
 * @param name the name of the component
 * @param bytes its size in bytes
 * @param depth 0 for a component of the data structure, 1 for a part of a component of depth 0, ...
 */
inline void log_component(const std::string& name, uint64_t bytes, uint64_t depth = 0)
{
    log_out() << std::string(2 * (depth + listing_indent), ' ') << name << ": " << format_share(bytes) << '\n';
    if (depth == 0) listing_listed += bytes;
}

// ends a listing started with begin_listing: logs the size not covered by the listed components ("other: ...")
inline void end_listing()
{
    if (listing_total > listing_listed) log_component("other", listing_total - listing_listed);
    listing_indent = 0;
    log_out() << std::flush;
    listing_total = 0;
    listing_listed = 0;
}

/**
 * @brief returns the time difference (in nanoseconds) between two time points
 * @param t1 the earlier time point
 * @param t2 the later time point
 * @return the time difference t2 - t1 in nanoseconds
 */
static uint64_t time_diff_ns(std::chrono::steady_clock::time_point t1, std::chrono::steady_clock::time_point t2)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
}

/**
 * @brief returns the time difference (in nanoseconds) between a time point and now
 * @param t the earlier time point
 * @return the time difference now - t in nanoseconds
 */
inline uint64_t time_diff_ns(std::chrono::steady_clock::time_point t) { return time_diff_ns(t, std::chrono::steady_clock::now()); }

// whether the line of a phase started with log_phase_start is open, i.e. its runtime has not been printed yet
inline bool phase_line_open = false;

// guards deferred_notes
inline std::mutex notes_lock;
// the lines logged with log_note while a phase line was open, in their order; printed after the phase line
inline std::vector<std::string> deferred_notes;

// prints and clears the lines deferred by log_note
inline void print_deferred_notes()
{
    std::lock_guard<std::mutex> guard(notes_lock);
    for (const std::string& note : deferred_notes) log_out() << note << '\n';
    deferred_notes.clear();
    log_out().flush();
}

/**
 * @brief logs a line of information; if the line of a phase is open, the line is printed after it ends
 * @param log whether logging is enabled
 * @param note the line (without a trailing newline)
 */
inline void log_note(bool log, const std::string& note)
{
    if (!log) return;

    if (phase_line_open) {
        std::lock_guard<std::mutex> guard(notes_lock);
        deferred_notes.push_back(note);
    } else {
        log_out() << note << std::endl;
    }
}

// ends the line of an open phase (before an error message is printed) and prints the deferred notes
inline void end_phase_line()
{
    if (phase_line_open) log_out() << std::endl;
    phase_line_open = false;
    print_deferred_notes();
}

/**
 * @brief logs the runtime between two time points, which ends the line of the phase, and returns the current time
 *        point
 * @param t1 the earlier time point
 * @param t2 the later time point
 * @return the current time point
 */
static std::chrono::steady_clock::time_point log_runtime(std::chrono::steady_clock::time_point t1, std::chrono::steady_clock::time_point t2)
{
    log_out() << ", in ~ " << format_time(time_diff_ns(t1, t2)) << std::endl;
    phase_line_open = false;
    if (!deferred_notes.empty()) print_deferred_notes();
    return std::chrono::steady_clock::now();
}

/**
 * @brief logs the runtime between a time point and now and returns the current time point
 * @param t the earlier time point
 * @return the current time point
 */
static std::chrono::steady_clock::time_point log_runtime(std::chrono::steady_clock::time_point t)
{
    return log_runtime(t, std::chrono::steady_clock::now());
}

// maximum of the peaks of malloc_count over all phases since the start of the current peak_scope (the peak
// of malloc_count is reset at the start of each phase)
inline uint64_t largest_phase_peak = 0;
// number of bytes allocated at the start of the current peak_scope; phase_peak() and phase_allocated() are
// relative to it
inline uint64_t peak_baseline = 0;

// updates largest_phase_peak with the current peak of malloc_count
inline void note_peak() { largest_phase_peak = std::max(largest_phase_peak, malloc_count_peak()); }

// returns the peak memory usage in bytes over all phases since the start of the current peak_scope
inline uint64_t overall_peak() { return std::max(largest_phase_peak, malloc_count_peak()); }

// returns the peak memory usage of the current phase in bytes, relative to peak_baseline
inline uint64_t phase_peak()
{
    uint64_t peak = malloc_count_peak();
    return peak > peak_baseline ? peak - peak_baseline : 0;
}

// returns the number of currently allocated bytes, relative to peak_baseline
inline uint64_t phase_allocated()
{
    uint64_t allocated = malloc_count_current();
    return allocated > peak_baseline ? allocated - peak_baseline : 0;
}

// number of peak_scope objects that currently exist
inline uint64_t peak_scope_depth = 0;

/**
 * @brief measures the peak memory usage of one construction: the constructor resets largest_phase_peak,
 *        peak_baseline and the peak of malloc_count. Scopes can be nested; the destructor merges the peak
 *        of the scope into the peak of the enclosing scope and restores its baseline.
 */
struct peak_scope {
    uint64_t enclosing_phase_peak; // largest_phase_peak of the enclosing scope (0 if there is none)
    uint64_t enclosing_baseline; // peak_baseline of the enclosing scope

    peak_scope()
    {
        note_peak();
        enclosing_phase_peak = peak_scope_depth == 0 ? 0 : largest_phase_peak;
        enclosing_baseline = peak_baseline;
        peak_scope_depth++;

        largest_phase_peak = 0;
        peak_baseline = malloc_count_current();
        malloc_count_reset_peak();
    }

    ~peak_scope()
    {
        note_peak();
        largest_phase_peak = std::max(largest_phase_peak, enclosing_phase_peak);
        peak_baseline = enclosing_baseline;
        if (peak_scope_depth > 0) peak_scope_depth--;
    }

    peak_scope(const peak_scope&) = delete;
    peak_scope& operator=(const peak_scope&) = delete;
};

/**
 * @brief appends the size of a file to the line of the running phase (call between log_phase_start and
 *        log_phase_end)
 * @param log whether logging is enabled
 * @param file path of the file
 */
inline void log_spilled_size(bool log, const std::string& file)
{
    if (!log) return;
    std::error_code ec;
    uint64_t bytes = std::filesystem::file_size(file, ec);
    if (!ec) log_out() << ": " << format_size(bytes) << std::flush;
}

// message of the running phase without leading newlines (see log_progress)
inline std::string running_phase;
// whether the line of the running phase currently shows a progress
inline bool phase_shows_progress = false;

// maximum number of parts of a phase that report their progress separately (see begin_progress)
inline constexpr uint64_t max_progress_parts = 4;

// progress of a finished phase; the progress is counted in per mille
inline constexpr uint64_t progress_full = 1000;

// guards the progress variables below
inline std::mutex progress_lock;
// [0..progress_parts-1] progress of each part of the running phase in per mille
inline std::array<uint64_t, max_progress_parts> progress_of_part = { 0, 0, 0, 0 };
// number of parts of the running phase
inline uint64_t progress_parts = 1;
// progress of the running phase in per mille that was printed last
inline uint64_t progress_drawn = 0;
// index of the part of the running phase the current thread reports the progress of
inline thread_local uint64_t progress_part = 0;
// time point at which the running phase started
inline std::chrono::steady_clock::time_point progress_started;
// number of characters of the progress line printed last
inline uint64_t progress_drawn_width = 0;

// duration in nanoseconds a phase must run before its progress is printed
inline constexpr uint64_t min_progress_ns = 1000000000;

/**
 * @brief resets the progress of the running phase and sets the number of its parts
 * @param parts number of parts that report their progress separately (clamped to [1, max_progress_parts])
 */
inline void begin_progress(uint64_t parts)
{
    std::lock_guard<std::mutex> guard(progress_lock);
    progress_parts = std::clamp<uint64_t>(parts, 1, max_progress_parts);
    progress_of_part.fill(0);
    progress_drawn = 0;
    progress_drawn_width = 0;
    progress_started = std::chrono::steady_clock::now();
}

/**
 * @brief starts a logged phase: prints msg, (re)sets the phase timer to now() and resets the peak of
 *        malloc_count and the progress
 * @param log whether logging is enabled
 * @param time phase timer; set to now() (only when logging)
 * @param msg message describing the phase
 */
inline void log_phase_start(bool log, std::chrono::steady_clock::time_point& time, const std::string& msg)
{
    if (log) {
        time = now();
        note_peak();
        malloc_count_reset_peak();
        // the leading newlines of msg are not printed again when the line is redrawn
        const size_t msg_beg = msg.find_first_not_of('\n');
        running_phase = msg_beg == std::string::npos ? std::string() : msg.substr(msg_beg);
        phase_shows_progress = false;
        begin_progress(1);
        phase_line_open = !msg.empty() && msg.back() != '\n';
        log_out() << msg << std::flush;
    }
}

// returns whether the log stream (see log_stream) is a terminal (only then the progress line is redrawn in place)
inline bool is_log_terminal()
{
    static const bool stdout_terminal = platform::is_terminal(stdout);
    static const bool stderr_terminal = platform::is_terminal(stderr);
    return log_stream == &std::cerr ? stderr_terminal : stdout_terminal;
}

/**
 * @brief sets the progress of the part of the running phase the calling thread reports for (see
 *        progress_part) and prints the progress of the phase (the mean over its parts) with the elapsed
 *        time and the estimated remaining time. The remaining time is estimated as (1 - f) / f * t,
 *        where f is the fraction of the phase that is done and t is the elapsed time.
 * @param log whether logging is enabled
 * @param permille [0..progress_full] progress of the part in per mille
 */
inline void log_progress(bool log, uint64_t permille)
{
    if (!log) return;
    std::lock_guard<std::mutex> guard(progress_lock);
    progress_of_part[progress_part < max_progress_parts ? progress_part : 0]
        = std::min(permille, progress_full);
    // progress of the phase in per mille (mean over its parts)
    const uint64_t done = std::accumulate(progress_of_part.begin(),
        progress_of_part.begin() + progress_parts, uint64_t(0)) / progress_parts;
    if (done == progress_drawn && phase_shows_progress) return;

    const uint64_t elapsed = time_diff_ns(progress_started);
    if (elapsed < min_progress_ns) return;

    std::string state = ' ' + std::to_string(done / 10) + '%';
    // the remaining time is printed once 1 % of the phase is done
    if (done >= 10 && done < progress_full)
        state += ", elapsed: " + format_eta(elapsed)
            + ", ETA " + format_eta((elapsed * (progress_full - done)) / done);

    // on a terminal, redraw the line of the phase
    if (is_log_terminal()) {
        const uint64_t before_width = progress_drawn_width;
        progress_drawn = done;
        phase_shows_progress = true;
        progress_drawn_width = running_phase.size() + state.size();
        // overwrite the remainder of a longer previous line with blanks
        const std::string blanks = progress_drawn_width < before_width
            ? std::string(before_width - progress_drawn_width, ' ') : std::string();
        log_out() << '\r' << running_phase << state << blanks << std::flush;
        return;
    }

    const uint64_t before = progress_drawn;
    progress_drawn = done;

    // else, append the progress each time another 10 % of the phase are done
    if (done / 100 > before / 100) {
        phase_shows_progress = true;
        log_out() << ' ' << (done / 100) * 10 << '%'
                  << (done >= 10 && done < progress_full
                         ? " (elapsed: " + format_eta(elapsed)
                             + ", ETA " + format_eta((elapsed * (progress_full - done)) / done) + ")"
                         : std::string())
                  << std::flush;
    }
}

/**
 * @brief marks the part of the running phase the calling thread reports for as finished; once all parts
 *        are finished, removes the progress from the line of the phase (only on a terminal), such that
 *        only the message of the phase remains
 * @param log whether logging is enabled
 */
inline void log_progress_done(bool log)
{
    std::lock_guard<std::mutex> guard(progress_lock);
    if (!log || !phase_shows_progress) return;

    progress_of_part[progress_part < max_progress_parts ? progress_part : 0] = progress_full;
    // minimum progress over all parts
    const uint64_t min_progress = *std::min_element(progress_of_part.begin(),
        progress_of_part.begin() + progress_parts);
    if (min_progress < progress_full) return;

    phase_shows_progress = false;
    if (!is_log_terminal()) return;
    const uint64_t width = progress_drawn_width > running_phase.size()
        ? progress_drawn_width - running_phase.size() : 0;
    progress_drawn_width = 0;
    log_out() << '\r' << running_phase << std::string(width, ' ')
              << '\r' << running_phase << std::flush;
}

// the default number of times phase_progress::at reports the progress over a phase
inline constexpr uint64_t default_progress_reports = 4096;

/**
 * @brief counts the steps done in a phase and prints its progress (see log_progress); the steps can be
 *        counted by multiple threads. The loop of the phase calls at(i) with its index (then grain() steps
 *        are counted at every grain()-th index), advance() with a number of steps or reached() with the
 *        total number of steps done.
 */
class phase_progress {
    bool active; // whether the progress is counted and printed
    uint64_t total; // total number of steps of the phase
    uint64_t reports; // the number of times the progress is reported by at() over the whole phase (at least)
    uint64_t mask; // grain() - 1, where grain() is a power of two
    std::atomic<uint64_t> done = 0; // number of steps done
    std::atomic<uint64_t> drawn = 0; // progress in per mille last passed to log_progress

    // sets mask such that grain() is the largest power of two <= total / reports (at least 1)
    void set_grain()
    {
        const uint64_t grain = std::max<uint64_t>(1, total / std::max<uint64_t>(reports, 1));
        mask = (uint64_t(1) << (std::bit_width(grain) - 1)) - 1;
    }

    /**
     * @brief adds steps to the number of steps done and prints the progress, if its value in per mille
     *        has increased
     * @param steps number of steps
     */
    void tick(uint64_t steps)
    {
        const uint64_t reached = done.fetch_add(steps, std::memory_order_relaxed) + steps;
        const uint64_t permille = std::min(progress_full, (reached * progress_full) / total);
        uint64_t before = drawn.load(std::memory_order_relaxed);

        // only the thread that increases drawn prints the progress
        while (permille > before) {
            if (drawn.compare_exchange_weak(before, permille, std::memory_order_relaxed)) {
                log_progress(true, permille);
                return;
            }
        }
    }

public:
    /**
     * @brief constructs a progress counter for a phase
     * @param log whether logging is enabled (if not, nothing is counted)
     * @param total total number of steps of the phase
     * @param reports grain() is set to the largest power of two <= total / reports
     */
    phase_progress(bool log, uint64_t total, uint64_t reports = default_progress_reports)
        : active(log && total != 0), total(std::max<uint64_t>(total, 1)), reports(reports)
    {
        set_grain();
    }

    phase_progress(const phase_progress&) = delete;
    phase_progress& operator=(const phase_progress&) = delete;

    /**
     * @brief sets the total number of steps of the phase and resets the number of steps done; must be
     *        called before the first step is counted
     * @param total total number of steps of the phase
     */
    void set_total(uint64_t total)
    {
        this->total = std::max<uint64_t>(total, 1);
        active = active && total != 0;
        set_grain();
        done.store(0, std::memory_order_relaxed);
        drawn.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief counts grain() steps, if i is the last index of a block of grain() consecutive indices; to
     *        be called with every index of a loop whose index counts the steps of the phase
     * @param i loop index
     */
    void at(uint64_t i)
    {
        if (active && (i & mask) == mask) tick(mask + 1);
    }

    /**
     * @brief counts a number of steps
     * @param steps number of steps done since the last call
     */
    void advance(uint64_t steps = 1)
    {
        if (active && steps != 0) tick(steps);
    }

    /**
     * @brief sets the number of steps done, if it is larger than the current number
     * @param reached total number of steps done
     */
    void reached(uint64_t reached)
    {
        if (!active) return;
        uint64_t before = done.load(std::memory_order_relaxed);
        while (reached > before
            && !done.compare_exchange_weak(before, reached, std::memory_order_relaxed)) { }
        if (reached > before) tick(0);
    }

    // returns the number of steps counted per report of at(); a loop that counts its own steps should
    // call advance() once per grain() steps
    uint64_t grain() const { return mask + 1; }

    // ends the progress output of the phase (see log_progress_done)
    void finish()
    {
        if (!active) return;
        active = false;
        log_progress_done(true);
    }

    ~phase_progress() { finish(); }
};

/**
 * @brief a stream buffer that passes the bytes read or written through another stream buffer and counts them as the
 *        steps of a phase_progress
 */
class progress_streambuf : public std::streambuf {
    std::streambuf* target; // the stream buffer the bytes are passed through
    phase_progress* progress; // counts the bytes
    uint64_t pending = 0; // the number of bytes passed through and not yet counted

    // counts bytes bytes, once at least progress->grain() bytes have been passed through
    void count(uint64_t bytes)
    {
        pending += bytes;

        if (pending >= progress->grain()) {
            progress->advance(pending);
            pending = 0;
        }
    }

protected:
    int_type underflow() override { return target->sgetc(); }

    int_type uflow() override
    {
        const int_type c = target->sbumpc();
        if (!traits_type::eq_int_type(c, traits_type::eof())) count(1);
        return c;
    }

    // reads n bytes in pieces of progress->grain() bytes and counts each piece
    std::streamsize xsgetn(char* s, std::streamsize n) override
    {
        std::streamsize read = 0;

        while (read < n) {
            const std::streamsize piece = std::min<std::streamsize>(n - read, std::streamsize(progress->grain()));
            const std::streamsize got = target->sgetn(s + read, piece);
            count(uint64_t(got));
            read += got;
            if (got < piece) break;
        }

        return read;
    }

    std::streamsize showmanyc() override { return target->in_avail(); }

    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const int_type written = target->sputc(traits_type::to_char_type(c));
        if (!traits_type::eq_int_type(written, traits_type::eof())) count(1);
        return written;
    }

    // writes n bytes in pieces of progress->grain() bytes and counts each piece
    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        std::streamsize written = 0;

        while (written < n) {
            const std::streamsize piece = std::min<std::streamsize>(n - written, std::streamsize(progress->grain()));
            const std::streamsize put = target->sputn(s + written, piece);
            count(uint64_t(put));
            written += put;
            if (put < piece) break;
        }

        return written;
    }

    int sync() override { return target->pubsync(); }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override
    {
        return target->pubseekoff(off, dir, which);
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override { return target->pubseekpos(pos, which); }

public:
    /**
     * @param stream the stream whose stream buffer the bytes are passed through
     * @param progress counts the bytes
     */
    progress_streambuf(std::ios& stream, phase_progress& progress) : target(stream.rdbuf()), progress(&progress) {}

    ~progress_streambuf() override { progress->advance(pending); }
};

/** @brief an input stream that reads from another stream and counts the bytes read (see progress_streambuf) */
class progress_istream : public std::istream {
    progress_streambuf buf; // passes the bytes through

public:
    /**
     * @param in the stream to read from
     * @param progress counts the bytes read
     */
    progress_istream(std::istream& in, phase_progress& progress) : std::istream(nullptr), buf(in, progress)
    {
        rdbuf(&buf);
    }
};

/** @brief an output stream that writes to another stream and counts the bytes written (see progress_streambuf) */
class progress_ostream : public std::ostream {
    progress_streambuf buf; // passes the bytes through

public:
    /**
     * @param out the stream to write to
     * @param progress counts the bytes written
     */
    progress_ostream(std::ostream& out, phase_progress& progress) : std::ostream(nullptr), buf(out, progress)
    {
        rdbuf(&buf);
    }

    ~progress_ostream() override { flush(); }
};

/**
 * @brief ends a logged phase: optionally writes the elapsed time to the measurement
 *        stream mf under the key mf_key (and phase_peak() and phase_allocated() under the keys
 *        peak_<phase> and held_<phase>, where <phase> is mf_key without the prefix "time_"), then logs the runtime
 *        and resets the timer
 * @param log whether logging is enabled
 * @param time phase timer; read for the elapsed time, then reset by log_runtime
 * @param mf measurement-file stream (nullptr => nothing written)
 * @param mf_key measurement-file key for the elapsed time (empty => nothing written)
 */
inline void log_phase_end(bool log, std::chrono::steady_clock::time_point& time, std::ostream* mf = nullptr, const std::string& mf_key = "")
{
    if (log) {
        if (!mf_key.empty() && mf != nullptr) {
            constexpr std::string_view time_prefix = "time_";
            const std::string phase = mf_key.starts_with(time_prefix) ? mf_key.substr(time_prefix.size()) : mf_key;
            *mf << " " << mf_key << "=" << time_diff_ns(time, now());
            *mf << " peak_" << phase << "=" << phase_peak();
            *mf << " held_" << phase << "=" << phase_allocated();
        }

        note_peak();
        time = log_runtime(time);
    }
}

/**
 * @brief logs "<label>: <peak> (<share> of the input)"
 * @param label the label, e.g. "peak memory usage"
 * @param peak_bytes the peak memory usage in bytes
 * @param input_bytes the size of the input in bytes (0: the share is not printed)
 */
inline void log_peak_memory_usage(const std::string& label, uint64_t peak_bytes, uint64_t input_bytes)
{
    log_out() << label << ": " << format_size(peak_bytes);
    if (input_bytes != 0) log_out() << " (" << format_percentage(peak_bytes, input_bytes) << " of the input)";
    log_out() << std::endl;
}

/**
 * @brief logs the start of a run: "<action> <input> (<size>) with <p> threads[, <parameters>]" and, if path_output is
 *        not empty, "output: <path_output>"
 * @param action what is done to the input, e.g. "compressing"
 * @param input the input, e.g. its path
 * @param input_bytes the size of the input in bytes
 * @param threads the number of threads
 * @param parameters the parameters as "<option> = <value>[ <unit>]", separated by ", " (empty: none)
 * @param path_output the path of the output file (empty: none)
 */
inline void log_run_start(const std::string& action, const std::string& input, uint64_t input_bytes, uint64_t threads,
    const std::string& parameters = "", const std::string& path_output = "")
{
    log_out() << action << " " << input << " (" << format_size(input_bytes) << ") with " << format_threads(threads);
    if (!parameters.empty()) log_out() << ", " << parameters;
    log_out() << std::endl;
    if (!path_output.empty()) log_out() << "output: " << path_output << std::endl;
}

// the value of the peak memory usage passed to log_construction_summary if it was not measured
inline constexpr uint64_t peak_not_measured = std::numeric_limits<uint64_t>::max();

/**
 * @brief logs the summary of a construction: "construction time: <time> (<throughput>)" and, if the peak memory usage
 *        was measured, "peak memory usage: <peak> (<share> of the input)"
 * @param time_ns the construction time in nanoseconds
 * @param input_bytes the size of the input in bytes
 * @param peak_bytes the peak memory usage in bytes (peak_not_measured: not printed)
 * @param with_throughput whether the throughput is printed (not if a part of the construction was read from a cache)
 */
inline void log_construction_summary(uint64_t time_ns, uint64_t input_bytes, uint64_t peak_bytes,
    bool with_throughput = true)
{
    log_out() << "construction time: " << format_time(time_ns);
    if (with_throughput && input_bytes != 0) log_out() << " (" << format_throughput(input_bytes, time_ns) << ")";
    log_out() << std::endl;
    if (peak_bytes != peak_not_measured) log_peak_memory_usage("peak memory usage", peak_bytes, input_bytes);
}

/**
 * @brief logs the size of the output of a construction: "<name> size: <size> (<share> of the input)"
 * @param name the name of the output, e.g. "index"
 * @param output_bytes the size of the output in bytes
 * @param input_bytes the size of the input in bytes
 */
inline void log_output_size(const std::string& name, uint64_t output_bytes, uint64_t input_bytes)
{
    log_out() << name << " size: " << format_size(output_bytes) << " ("
              << format_percentage(output_bytes, input_bytes) << " of the input)" << std::endl;
}

/**
 * @brief prints a message to the log stream (see log_stream)
 * @param message the message to print
 */
inline void log_message(std::string message) { log_out() << message << std::flush; }

/**
 * @brief prints message (without resetting any phase timer), if logging is enabled;
 *        use for phase-start messages that must not reset the timer and for section headers
 * @param log whether logging is enabled
 * @param message message to print (include a trailing "\n" for a header line)
 */
inline void log_message(bool log, const std::string& message) { if (log) log_out() << message << std::flush; }

#endif // STRING_UTILS_MISC_LOG_HPP
