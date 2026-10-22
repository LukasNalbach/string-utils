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

#ifndef STRING_UTILS_MISC_THREADS_HPP
#define STRING_UTILS_MISC_THREADS_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <omp.h>

#include <misc/log.hpp>
#include <misc/platform.hpp>

// the CPUs the process may run on, read before main (before OpenMP binds a thread to a place)
inline const platform::cpu_mask startup_cpus = platform::process_cpus();

// the CPUs of the innermost full_affinity in scope on the calling thread (nullptr if there is none)
inline thread_local const platform::cpu_mask* scoped_cpus = nullptr;

/**
 * @brief returns the CPUs the process may run on: the CPUs it could run on when it started and the CPUs of the OpenMP
 *        places (OMP_PLACES)
 */
inline const platform::cpu_mask& process_cpus()
{
    static const platform::cpu_mask cpus = [] {
        platform::cpu_mask all = startup_cpus.count() > 0 ? startup_cpus : platform::process_cpus();

        for (int place = 0; place < omp_get_num_places(); place++) {
            std::vector<int> ids(omp_get_place_num_procs(place));
            omp_get_place_proc_ids(place, ids.data());
            for (int id : ids) all.set(uint64_t(id));
        }

        return all;
    }();

    return cpus;
}

/**
 * @brief returns the CPUs the calling thread may run on: those of the innermost full_affinity in scope on the thread,
 *        or all CPUs of the process (see process_cpus)
 */
inline const platform::cpu_mask& allowed_cpus() { return scoped_cpus != nullptr ? *scoped_cpus : process_cpus(); }

/**
 * @brief sets the affinity of the calling thread to a set of CPUs, if it is not already that set
 * @param cpus the CPUs (by default the CPUs the thread may run on, see allowed_cpus)
 * @return whether the affinity has been changed
 */
inline bool set_affinity_to_allowed_cpus(const platform::cpu_mask& cpus = allowed_cpus())
{
    platform::cpu_mask cur_cpus;
    if (!platform::get_thread_cpus(cur_cpus) || cur_cpus == cpus) return false;
    return platform::set_thread_cpus(cpus);
}

/**
 * @brief sets the affinity of the calling thread to a set of CPUs while in scope and restores it afterwards; while in
 *        scope, the set is also the set of CPUs the thread may run on (see allowed_cpus); threads not created by OpenMP
 *        and child processes started meanwhile inherit the affinity, other threads the code in scope starts get it
 *        with full_affinity(cpus())
 */
class full_affinity {
    platform::cpu_mask _cpus; // the CPUs the calling thread runs on while in scope
    platform::cpu_mask _prev_cpus; // the CPUs the calling thread was allowed to run on before
    const platform::cpu_mask* _prev_scoped_cpus; // the CPUs of the enclosing full_affinity (see scoped_cpus)
    bool _restore = false; // true <=> the affinity has been changed and is restored by the destructor

public:
    // sets the affinity to the CPUs the calling thread may run on (see allowed_cpus)
    full_affinity() : full_affinity(allowed_cpus()) { }

    // sets the affinity to a set of CPUs
    explicit full_affinity(const platform::cpu_mask& cpus) : _cpus(cpus), _prev_scoped_cpus(scoped_cpus)
    {
        scoped_cpus = &_cpus;
        if (platform::get_thread_cpus(_prev_cpus)) _restore = set_affinity_to_allowed_cpus(_cpus);
    }

    full_affinity(const full_affinity&) = delete;
    full_affinity& operator=(const full_affinity&) = delete;

    ~full_affinity()
    {
        if (_restore) platform::set_thread_cpus(_prev_cpus);
        scoped_cpus = _prev_scoped_cpus;
    }

    // returns the CPUs the calling thread runs on while in scope
    const platform::cpu_mask& cpus() const { return _cpus; }
};

/**
 * @brief returns the CPUs each thread of an OpenMP parallel region may run on (determined by OMP_PLACES,
 *        OMP_PROC_BIND and the affinity of the process)
 * @param num_threads number of threads of the region
 * @return [0..t-1] the CPU set of each thread, where t is the number of threads the region was run with
 */
inline std::vector<platform::cpu_mask> omp_thread_cpus(uint64_t num_threads)
{
    std::vector<platform::cpu_mask> cpus(std::max<uint64_t>(1, num_threads));
    uint64_t num_team_threads = 1;

    #pragma omp parallel num_threads(cpus.size())
    {
        if (omp_get_thread_num() == 0) num_team_threads = omp_get_num_threads();
        platform::get_thread_cpus(cpus[omp_get_thread_num()]);
    }

    cpus.resize(num_team_threads);
    return cpus;
}

/**
 * @brief returns the number of physical cores the threads of an OpenMP parallel region may run on (CPUs of the same
 *        core, e.g., hyper-threads, count once; see omp_thread_cpus), at most the number of threads
 * @param num_threads number of threads of the region
 */
inline uint64_t num_cores_of_omp_team(uint64_t num_threads)
{
    platform::cpu_mask team_cpus;
    for (const platform::cpu_mask& cpus : omp_thread_cpus(num_threads)) team_cpus |= cpus;

    // the cores, each given by a string naming it (see platform::core_of_cpu); a CPU without topology information
    // counts as a core of its own
    std::set<std::string> cores;

    for (uint64_t cpu = 0; cpu < team_cpus.end(); cpu++) {
        if (team_cpus.test(cpu)) cores.insert(platform::core_of_cpu(cpu));
    }

    return std::clamp<uint64_t>(cores.size(), 1, std::max<uint64_t>(1, num_threads));
}

// rethrows the first exception in failures that is not nullptr
inline void rethrow_first_exception(const std::vector<std::exception_ptr>& failures)
{
    for (const std::exception_ptr& failure : failures) if (failure != nullptr) std::rethrow_exception(failure);
}

/**
 * @brief runs fnc(t, p) on each thread t of an OpenMP team of p <= num_threads threads; after all threads have
 *        finished, the exception of the thread with the smallest t that threw one is rethrown
 * @param num_threads the number of threads requested for the team
 * @param fnc the function run on each thread
 */
template <typename fnc_t>
inline void run_on_threads(uint64_t num_threads, fnc_t fnc)
{
    std::vector<std::exception_ptr> failures(num_threads);

    #pragma omp parallel num_threads(num_threads)
    {
        const uint64_t t = omp_get_thread_num();

        try {
            fnc(t, uint64_t(omp_get_num_threads()));
        } catch (...) {
            failures[t] = std::current_exception();
        }
    }

    rethrow_first_exception(failures);
}

/**
 * @brief runs tasks at the same time, each on a thread of an OpenMP team; the thread of a task runs on the CPUs the
 *        calling thread may run on (see full_affinity and allowed_cpus), and nested parallel regions are enabled while
 *        the tasks run; after all tasks have finished, the exception of the first task that threw one is rethrown
 * @param tasks the tasks; task k is called as tasks[k]()
 */
inline void run_concurrently(const std::vector<std::function<void()>>& tasks)
{
    if (tasks.empty()) return;
    std::vector<std::exception_ptr> failures(tasks.size());
    const platform::cpu_mask& cpus = allowed_cpus();
    const int max_levels = omp_get_max_active_levels();
    omp_set_max_active_levels(omp_get_supported_active_levels());

    #pragma omp parallel num_threads(tasks.size())
    {
        const full_affinity affinity(cpus);

        // the team can have fewer threads than there are tasks
        for (uint64_t k = omp_get_thread_num(); k < tasks.size(); k += omp_get_num_threads()) {
            try { tasks[k](); }
            catch (...) { failures[k] = std::current_exception(); }
        }
    }

    omp_set_max_active_levels(max_levels);
    rethrow_first_exception(failures);
}

/**
 * @brief runs tasks on at most max_concurrent threads at the same time (see run_concurrently): each thread runs the
 *        next task that has not been started; while task k runs, its thread reports its progress to part k of the
 *        progress line (progress_part = k, see log_progress); after all tasks have finished, the exception of the
 *        first task that threw one is rethrown
 * @param tasks the tasks; task k is called as tasks[k](); at most max_progress_parts tasks report progress
 * @param max_concurrent the maximum number of tasks that run at the same time
 */
inline void run_tasks_with_progress_parts(const std::vector<std::function<void()>>& tasks, uint64_t max_concurrent)
{
    std::vector<std::exception_ptr> failures(tasks.size());
    std::atomic<uint64_t> next { 0 };

    auto run_next_tasks = [&]() {
        for (uint64_t k = next++; k < tasks.size(); k = next++) {
            progress_part = k;
            try { tasks[k](); }
            catch (...) { failures[k] = std::current_exception(); }
        }
    };

    const uint64_t num_threads = std::min<uint64_t>(max_concurrent, tasks.size());
    run_concurrently(std::vector<std::function<void()>>(num_threads, run_next_tasks));
    rethrow_first_exception(failures);
}

#endif // STRING_UTILS_MISC_THREADS_HPP
