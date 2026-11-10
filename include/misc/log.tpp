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
 * @file log.tpp
 * @brief the formatting functions of log.hpp and the methods of its progress and peak classes
 */

#ifndef STRING_UTILS_MISC_LOG_TPP
#define STRING_UTILS_MISC_LOG_TPP

#include <misc/log.hpp>

static inline std::string format_time(uint64_t ns)
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

inline std::string format_ratio(double value)
{
    std::ostringstream str;
    str << std::fixed << std::setprecision(ratio_decimals) << value;
    std::string formatted = str.str();

    if (formatted.find('.') != std::string::npos) {
        while (formatted.back() == '0') formatted.pop_back();
        if (formatted.back() == '.') formatted.pop_back();
    }

    // thousands separators in the integer part, as in format_number
    const uint64_t digits_beg = formatted.front() == '-' ? 1 : 0;
    const uint64_t digits_end = std::min(formatted.find('.'), formatted.size());
    for (uint64_t i = digits_end; i > digits_beg + 3; i -= 3) formatted.insert(i - 3, ",");
    return formatted;
}

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

inline std::string format_share(uint64_t B)
{
    std::string size_str = format_size(B);
    if (listing_total == 0) return size_str;

    std::ostringstream share;
    share << size_str << " (" << std::fixed << std::setprecision(1)
          << (100.0 * double(B) / double(listing_total)) << " %)";
    return share.str();
}

inline void log_component(const std::string& name, uint64_t bytes, uint64_t depth)
{
    const uint64_t level = depth + listing_indent;
    const std::string line = std::string(2 * level, ' ') + name + ": " + format_share(bytes) + "\n";

    // a component below the heading of the open group belongs to the group, a component at level 0 ends the group
    if (!component_group.title.empty()) {
        if (level > 0) {
            if (level == 1) component_group.bytes += bytes;
            component_group.lines += line;
            return;
        }

        end_component_group();
    }

    log_out() << line;
    log_last_line_empty = false;
}

inline void print_deferred_notes()
{
    std::lock_guard<std::mutex> guard(notes_lock);
    if (deferred_notes.empty()) return;
    for (const std::string& note : deferred_notes) log_out() << note << '\n';
    log_last_line_empty = false;
    deferred_notes.clear();
    log_out().flush();
}

inline void log_note(bool log, const std::string& note)
{
    if (!log) return;

    if (phase_line_open) {
        std::lock_guard<std::mutex> guard(notes_lock);
        deferred_notes.push_back(note);
    } else {
        log_out() << note << std::endl;
        log_last_line_empty = false;
    }
}

inline void end_phase_line()
{
    if (phase_line_open) {
        log_out() << std::endl;
        log_last_line_empty = false;
    }

    phase_line_open = false;
    print_deferred_notes();
}

inline void log_empty_line(bool log)
{
    if (!log) return;
    end_phase_line();
    // the pending line of log_grouped is printed first
    flush_log_group();
    if (!log_last_line_empty) log_out() << std::endl;
    log_last_line_empty = true;
}

inline void log_block_begin(bool log, const std::string& title)
{
    if (!log) return;
    log_empty_line(true);
    log_out() << title << ":" << std::endl;
    // no empty line follows the title directly
    log_last_line_empty = true;
}

static inline std::chrono::steady_clock::time_point log_runtime(std::chrono::steady_clock::time_point t1, std::chrono::steady_clock::time_point t2)
{
    log_out() << ", in ~ " << format_time(time_diff_ns(t1, t2)) << std::endl;
    phase_line_open = false;
    log_last_line_empty = false;
    if (!deferred_notes.empty()) print_deferred_notes();
    return std::chrono::steady_clock::now();
}

inline peak_scope::peak_scope()
{
    note_peak();
    enclosing_phase_peak = peak_scope_depth == 0 ? 0 : largest_phase_peak;
    enclosing_baseline = peak_baseline;
    peak_scope_depth++;

    largest_phase_peak = 0;
    peak_baseline = malloc_count_current();
    malloc_count_reset_peak();
}

inline void begin_progress(uint64_t parts)
{
    std::lock_guard<std::mutex> guard(progress_lock);
    progress_parts = std::clamp<uint64_t>(parts, 1, max_progress_parts);
    progress_of_part.fill(0);
    progress_drawn = 0;
    progress_drawn_width = 0;
    progress_started = std::chrono::steady_clock::now();
}

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
        phase_detail_added = false;

        // with an empty message, the phase is started without output (see log_grouped)
        if (!msg.empty()) {
            log_last_line_empty = false;
            log_out() << msg << std::flush;
        }
    }
}

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

inline void log_phase_detail(bool log, const std::string& detail)
{
    if (!log) return;
    std::lock_guard<std::mutex> guard(progress_lock);
    if (phase_detail_added || !phase_line_open) return;
    phase_detail_added = true;

    if (phase_shows_progress && is_log_terminal()) {
        // overwrite the progress with the detail and blanks, then return to the end of the message
        const uint64_t width = progress_drawn_width > running_phase.size() + detail.size()
            ? progress_drawn_width - running_phase.size() - detail.size() : 0;
        running_phase += detail;
        log_out() << '\r' << running_phase << std::string(width, ' ') << '\r' << running_phase << std::flush;
        progress_drawn_width = 0;
        phase_shows_progress = false;
        return;
    }

    running_phase += detail;
    log_out() << detail << std::flush;
}

inline void phase_progress::tick(uint64_t steps)
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

inline void phase_progress::set_total(uint64_t total)
{
    this->total = std::max<uint64_t>(total, 1);
    active = active && total != 0;
    set_grain();
    done.store(0, std::memory_order_relaxed);
    drawn.store(0, std::memory_order_relaxed);
}

inline void phase_progress::reached(uint64_t reached)
{
    if (!active) return;
    uint64_t before = done.load(std::memory_order_relaxed);
    while (reached > before
        && !done.compare_exchange_weak(before, reached, std::memory_order_relaxed)) { }
    if (reached > before) tick(0);
}

inline void progress_streambuf::count(uint64_t bytes)
{
    pending += bytes;

    if (pending >= progress->grain()) {
        progress->advance(pending);
        pending = 0;
    }
}

inline std::streamsize progress_streambuf::xsgetn(char* s, std::streamsize n)
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

inline std::streamsize progress_streambuf::xsputn(const char* s, std::streamsize n)
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

inline void log_phase_end(bool log, std::chrono::steady_clock::time_point& time, std::ostream* mf, const std::string& mf_key)
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

inline void flush_log_group()
{
    if (log_group.action.empty()) return;
    std::ostream& out = *log_stream;
    out << log_group.action << " ";

    // consecutive items of the same name are printed once with their qualifiers, e.g. "the samples (forward,
    // backward)"; then a qualifier is printed once after the last of consecutive items with it, e.g. "the samples,
    // the offsets (forward)"
    std::vector<std::pair<std::string, std::string>> merged;

    for (const auto& [name, qualifier] : log_group.items) {
        if (!merged.empty() && merged.back().first == name && !qualifier.empty() && !merged.back().second.empty())
            merged.back().second += ", " + qualifier;
        else merged.emplace_back(name, qualifier);
    }

    for (uint64_t i = 0; i < merged.size(); i++) {
        const auto& [name, qualifier] = merged[i];
        out << (i == 0 ? "" : ", ") << name;
        if (!qualifier.empty() && (i + 1 == merged.size() || merged[i + 1].second != qualifier))
            out << " (" << qualifier << ")";
    }

    if (log_group.with_size) out << ": " << format_size(log_group.bytes);
    out << ", in ~ " << format_time(log_group.ns) << std::endl;
    log_last_line_empty = false;
    log_group = log_group_t();
}

inline void log_grouped(bool log, const std::string& action, const std::string& item, uint64_t bytes, bool with_size, uint64_t ns)
{
    if (!log) return;
    end_phase_line();

    // splits the qualifier in parentheses off the end of the item
    const size_t open = item.size() > 1 && item.back() == ')' ? item.rfind(" (") : std::string::npos;
    std::pair<std::string, std::string> entry = open == std::string::npos
        ? std::pair<std::string, std::string>(item, std::string())
        : std::pair<std::string, std::string>(item.substr(0, open), item.substr(open + 2, item.size() - open - 3));

    // an item with another qualifier is printed in a new line, unless it has the name of the previous item
    if (!log_group.items.empty() && log_group.items.back().second != entry.second
        && log_group.items.back().first != entry.first) flush_log_group();

    if (log_group.action != action) {
        flush_log_group();
        log_group.action = action;
    }

    // an item equal to the previous one is printed once
    if (log_group.items.empty() || log_group.items.back() != entry) log_group.items.push_back(std::move(entry));
    log_group.bytes += bytes;
    log_group.with_size = log_group.with_size || with_size;
    log_group.ns += ns;
}

inline void log_grouped_phase_end(bool log, std::chrono::steady_clock::time_point& time, const std::string& message,
    uint64_t bytes, bool with_size, std::ostream* mf, const std::string& mf_key, bool logged)
{
    if (!log) return;
    const uint64_t ns = time_diff_ns(time, now());

    if (mf != nullptr && !mf_key.empty()) {
        constexpr std::string_view time_prefix = "time_";
        const std::string phase = mf_key.starts_with(time_prefix) ? mf_key.substr(time_prefix.size()) : mf_key;
        *mf << " " << mf_key << "=" << ns << " peak_" << phase << "=" << phase_peak() << " held_" << phase << "="
            << phase_allocated();
    }

    note_peak();
    const size_t space = message.find(' ');

    if (logged)
        log_grouped(true, message.substr(0, space), space == std::string::npos ? "" : message.substr(space + 1), bytes,
            with_size, ns);

    time = now();
}

inline void log_run_start(const std::string& action, const std::string& input, uint64_t input_bytes, uint64_t threads,
    const std::string& parameters, const std::string& path_output)
{
    log_out() << action << " " << input << " (" << format_size(input_bytes) << ")" << std::endl;
    log_out() << format_threads(threads);
    if (!parameters.empty()) log_out() << ", " << parameters;
    log_out() << std::endl;
    if (!path_output.empty()) log_out() << "output: " << path_output << std::endl;
    log_last_line_empty = false;
    log_empty_line(true);
}

inline void log_construction_summary(const std::string& name, uint64_t time_ns, uint64_t input_bytes,
    uint64_t peak_bytes, bool with_throughput, const std::string& source)
{
    log_block_begin(true, name + " built" + (source.empty() ? "" : " from " + source));
    log_out() << "construction time: " << format_time(time_ns);
    if (with_throughput && input_bytes != 0) log_out() << " (" << format_throughput(input_bytes, time_ns) << ")";
    log_out() << std::endl;
    if (peak_bytes != peak_not_measured)
        log_peak_memory_usage("peak memory usage", peak_bytes, input_bytes);
}

#endif // STRING_UTILS_MISC_LOG_TPP
