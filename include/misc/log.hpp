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

// prints the line of the items logged with log_grouped, if there are any (defined below)
inline void flush_log_group();

// returns the stream the log functions write to; first prints the pending line of log_grouped
inline std::ostream& log_out()
{
    flush_log_group();
    return *log_stream;
}

// whether the last line written by the log functions is empty or a block title (or nothing has been written yet), see
// log_empty_line and log_block_begin
inline bool log_last_line_empty = true;

// returns the current time point of the steady clock
static std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

/**
 * @brief formats a duration in nanoseconds with an appropriate unit (ns, us, ms, s, min or h)
 * @param ns a duration in nanoseconds
 * @return the formatted duration; from one minute on as minutes:seconds (e.g. "7:03 min") or
 *         hours:minutes (e.g. "2:41 h")
 */
static std::string format_time(uint64_t ns);

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
inline std::string format_ratio(double value);

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
inline std::string format_number(uint64_t value);

/**
 * @brief formats a count with its noun
 * @param count the count
 * @param noun the noun in the singular, e.g. "sequence"
 * @param plural the noun in the plural (empty: noun + "s")
 * @return "1 <noun>" or "<count> <plural>"
 */
inline std::string format_count(uint64_t count, const std::string& noun, const std::string& plural = "")
{
    return format_number(count) + " " + (count == 1 ? noun : plural.empty() ? noun + "s" : plural);
}

/**
 * @brief formats a number of threads
 * @param p the number of threads
 * @return "1 thread" or "<p> threads"
 */
inline std::string format_threads(uint64_t p) { return format_count(p, "thread"); }

/**
 * @brief formats a size in bytes with an appropriate unit (B, KB, MB or GB)
 * @param B a size in bytes
 * @return the formatted size (sizes in GB with two decimals)
 */
inline std::string format_size(uint64_t B);

// size in bytes of the data structure whose components are being listed (0 if no listing is in progress);
// set by begin_listing and reset to 0 by end_listing (see format_share)
inline uint64_t listing_total = 0;
// number of levels by which log_component indents the components in addition to their depth (e.g. 1 for the
// components listed under a heading)
inline uint64_t listing_indent = 0;

/**
 * @brief formats a size and, if listing_total is set, its share of listing_total
 * @param B a size in bytes
 * @return the formatted size, followed by its share in percent, e.g. "12 MB (3.4 %)"
 */
inline std::string format_share(uint64_t B);

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
    return true;
}

// the components listed below a heading whose size is the sum of their sizes (see begin_component_group)
struct component_group_t {
    std::string title; // the heading; empty: no group is open
    uint64_t bytes = 0; // the sum of the sizes of the components of the group at the first level below the heading
    std::string lines; // the lines of the components of the group
};

inline component_group_t component_group;

// prints the heading of the open group of components with their total size, followed by their lines, and closes it
inline void end_component_group()
{
    if (component_group.title.empty()) return;
    log_out() << component_group.title << ": " << format_share(component_group.bytes) << '\n' << component_group.lines;
    log_last_line_empty = false;
    component_group = component_group_t();
}

/**
 * @brief starts a group of components below a heading (and ends the open one): the components logged at a level > 0
 *        (depth plus listing_indent) up to the next component at level 0, the next group or end_listing are printed
 *        below the line "<title>: <size> (<share> %)", whose size is the sum of the components at level 1
 * @param title the heading, e.g. "forward index"
 */
inline void begin_component_group(const std::string& title)
{
    end_component_group();
    component_group.title = title;
}

/**
 * @brief logs the size of a component of the data structure whose sizes are being listed, with its share of
 *        listing_total: "<name>: <size> (<share> %)", indented by two spaces per depth (plus listing_indent)
 * @param name the name of the component
 * @param bytes its size in bytes
 * @param depth 0 for a component of the data structure, 1 for a part of a component of depth 0, ...
 */
inline void log_component(const std::string& name, uint64_t bytes, uint64_t depth = 0);

// ends a listing started with begin_listing
inline void end_listing()
{
    end_component_group();
    listing_indent = 0;
    log_out() << std::flush;
    listing_total = 0;
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
// whether a detail has been appended to the line of the running phase (see log_phase_detail)
inline bool phase_detail_added = false;

// guards deferred_notes
inline std::mutex notes_lock;
// the lines logged with log_note while a phase line was open, in their order; printed after the phase line
inline std::vector<std::string> deferred_notes;

// prints and clears the lines deferred by log_note
inline void print_deferred_notes();

/**
 * @brief logs a line of information; if the line of a phase is open, the line is printed after it ends
 * @param log whether logging is enabled
 * @param note the line (without a trailing newline)
 */
inline void log_note(bool log, const std::string& note);

// ends the line of an open phase (before an error message is printed) and prints the deferred notes
inline void end_phase_line();

/**
 * @brief writes an empty line that separates two blocks of log lines, unless the last line written by the log
 *        functions is empty
 * @param log whether logging is enabled
 */
inline void log_empty_line(bool log);

/**
 * @brief starts a block of log lines, e.g. the phases of a step: an empty line (see log_empty_line) and the title
 *        followed by a colon
 * @param log whether logging is enabled
 * @param title the title of the block, e.g. "building the index"
 */
inline void log_block_begin(bool log, const std::string& title);

// ends a block of log lines with an empty line (see log_empty_line)
inline void log_block_end(bool log) { log_empty_line(log); }

/**
 * @brief logs the runtime between two time points, which ends the line of the phase, and returns the current time
 *        point
 * @param t1 the earlier time point
 * @param t2 the later time point
 * @return the current time point
 */
static std::chrono::steady_clock::time_point log_runtime(std::chrono::steady_clock::time_point t1, std::chrono::steady_clock::time_point t2);

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

// message of the running phase without leading newlines (see log_progress)
inline std::string running_phase;

// updates largest_phase_peak with the current peak of malloc_count
inline void note_peak() { largest_phase_peak = std::max<uint64_t>(largest_phase_peak, malloc_count_peak()); }

// returns the peak memory usage in bytes over all phases since the start of the current peak_scope
inline uint64_t overall_peak() { return std::max<uint64_t>(largest_phase_peak, malloc_count_peak()); }

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

    peak_scope();

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
inline void begin_progress(uint64_t parts);

/**
 * @brief starts a logged phase: prints msg, (re)sets the phase timer to now() and resets the peak of
 *        malloc_count and the progress
 * @param log whether logging is enabled
 * @param time phase timer; set to now() (only when logging)
 * @param msg message describing the phase
 */
inline void log_phase_start(bool log, std::chrono::steady_clock::time_point& time, const std::string& msg);

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
inline void log_progress(bool log, uint64_t permille);

/**
 * @brief marks the part of the running phase the calling thread reports for as finished; once all parts
 *        are finished, removes the progress from the line of the phase (only on a terminal), such that
 *        only the message of the phase remains
 * @param log whether logging is enabled
 */
inline void log_progress_done(bool log);

/**
 * @brief appends a detail to the line of the running phase, e.g. " (8 cores with 15 threads)", if the line is open; only
 *        the first detail of a phase is appended. On a terminal, a drawn progress is removed and drawn again after the
 *        detail by the next report; else, the detail follows the progress appended to the line so far.
 * @param log whether logging is enabled
 * @param detail the detail
 */
inline void log_phase_detail(bool log, const std::string& detail);

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
    void tick(uint64_t steps);

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
    void set_total(uint64_t total);

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
    void reached(uint64_t reached);

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
    void count(uint64_t bytes);

protected:
    int_type underflow() override { return target->sgetc(); }

    int_type uflow() override
    {
        const int_type c = target->sbumpc();
        if (!traits_type::eq_int_type(c, traits_type::eof())) count(1);
        return c;
    }

    // reads n bytes in pieces of progress->grain() bytes and counts each piece
    std::streamsize xsgetn(char* s, std::streamsize n) override;

    std::streamsize showmanyc() override { return target->in_avail(); }

    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const int_type written = target->sputc(traits_type::to_char_type(c));
        if (!traits_type::eq_int_type(written, traits_type::eof())) count(1);
        return written;
    }

    // writes n bytes in pieces of progress->grain() bytes and counts each piece
    std::streamsize xsputn(const char* s, std::streamsize n) override;

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
inline void log_phase_end(bool log, std::chrono::steady_clock::time_point& time, std::ostream* mf = nullptr, const std::string& mf_key = "");

// the line of the items logged with log_grouped that is not printed yet
struct log_group_t {
    std::string action; // the action of the items, e.g. "storing"; empty: no pending line
    // the items, each split into its name and its qualifier in parentheses at its end, e.g. ("the samples", "forward")
    std::vector<std::pair<std::string, std::string>> items;
    uint64_t bytes = 0; // the total size of the items
    bool with_size = false; // whether the size is printed
    uint64_t ns = 0; // the total time of the items in nanoseconds
};

inline log_group_t log_group;

inline void flush_log_group();

/**
 * @brief logs an item of an action, e.g. storing a data structure, in one line with the items of the same action
 *        logged directly before it: "<action> <item>, <item>, ...[: <total size>], in ~ <total time>"; the line is
 *        printed before the next other output of the log functions (see log_out); a qualifier in parentheses at the
 *        end of an item is printed once for consecutive items with it, consecutive items of the same name are
 *        printed once with their qualifiers, an item equal to the previous one is printed once, and an item with
 *        another qualifier starts a new line
 * @param log whether logging is enabled
 * @param action the action, e.g. "storing"
 * @param item the item, e.g. "the samples (forward)"
 * @param bytes the size of the item
 * @param with_size whether the size is printed
 * @param ns the time of the item in nanoseconds
 */
inline void log_grouped(bool log, const std::string& action, const std::string& item, uint64_t bytes, bool with_size,
    uint64_t ns);

/**
 * @brief ends a phase started with log_phase_start(log, time, "") and logs it with log_grouped, and writes the
 *        measurement keys of log_phase_end
 * @param log whether logging is enabled
 * @param time the phase timer; set to now
 * @param message the action and the item, e.g. "storing the samples" (see log_grouped)
 * @param bytes the size of the item
 * @param with_size whether the size is printed
 * @param mf the measurement file (nullptr: none)
 * @param mf_key the measurement key of the phase (empty: none)
 * @param logged whether the phase is logged (else only the measurement keys are written)
 */
inline void log_grouped_phase_end(bool log, std::chrono::steady_clock::time_point& time, const std::string& message,
    uint64_t bytes, bool with_size, std::ostream* mf = nullptr, const std::string& mf_key = "", bool logged = true);

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
    log_last_line_empty = false;
}

/**
 * @brief logs the start of a run: "<action> <input> (<size>)", "<p> threads[, <parameters>]" and, if path_output is
 *        not empty, "output: <path_output>", followed by an empty line
 * @param action what is done to the input, e.g. "compressing"
 * @param input the input, e.g. its path
 * @param input_bytes the size of the input in bytes
 * @param threads the number of threads
 * @param parameters the parameters as "<option> = <value>[ <unit>]", separated by ", " (empty: none)
 * @param path_output the path of the output file (empty: none)
 */
inline void log_run_start(const std::string& action, const std::string& input, uint64_t input_bytes, uint64_t threads,
    const std::string& parameters = "", const std::string& path_output = "");

// the value of the peak memory usage passed to log_construction_summary if it was not measured
inline constexpr uint64_t peak_not_measured = std::numeric_limits<uint64_t>::max();

/**
 * @brief logs the summary of a construction as a block (see log_block_begin): "<name> built [from <source>]:",
 *        "construction time: <time> (<throughput>)" and, if the peak memory usage was measured, "peak memory usage:
 *        <peak> (<share> of the input)"
 * @param name what was built, e.g. "RLBWT"
 * @param time_ns the construction time in nanoseconds
 * @param input_bytes the size of the input in bytes
 * @param peak_bytes the peak memory usage in bytes (peak_not_measured: not printed)
 * @param with_throughput whether the throughput is printed (not if a part of the construction was read from a cache)
 * @param source what the construction started from, e.g. "the RLBWT" (empty: the input)
 */
inline void log_construction_summary(const std::string& name, uint64_t time_ns, uint64_t input_bytes,
    uint64_t peak_bytes, bool with_throughput = true, const std::string& source = "");

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
    log_last_line_empty = false;
}

/**
 * @brief prints a message to the log stream (see log_stream)
 * @param message the message to print
 */
inline void log_message(std::string message)
{
    if (!message.empty()) log_last_line_empty = false;
    log_out() << message << std::flush;
}

/**
 * @brief prints message (without resetting any phase timer), if logging is enabled;
 *        use for phase-start messages that must not reset the timer and for section headers
 * @param log whether logging is enabled
 * @param message message to print (include a trailing "\n" for a header line)
 */
inline void log_message(bool log, const std::string& message) { if (log) log_message(message); }

#include <misc/log.tpp>

#endif // STRING_UTILS_MISC_LOG_HPP
