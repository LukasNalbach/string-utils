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
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if defined(__linux__)
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

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
 * @brief makes the threads of an OpenMP parallel region run on the CPUs of the full_affinity in scope on the thread
 *        that starts the region (OpenMP places the threads by OMP_PLACES and OMP_PROC_BIND, those of a nested region on
 *        the places of the enclosing region, and ignores the affinity of the starting thread): declared before the
 *        region and passed to it as firstprivate, the copy of each thread sets the affinity of the thread while the
 *        region runs (see full_affinity); without a full_affinity in scope, the copies do nothing
 */
class region_affinity {
    const platform::cpu_mask* _cpus = scoped_cpus; // the CPUs of the full_affinity in scope on the starting thread
    std::optional<full_affinity> _affinity; // the affinity of the thread of a copy

public:
    region_affinity() = default;

    region_affinity(const region_affinity& other) : _cpus(other._cpus)
    {
        if (_cpus != nullptr) _affinity.emplace(*_cpus);
    }

    region_affinity& operator=(const region_affinity&) = delete;
};

/**
 * @brief writes a byte of each page of a buffer with the threads of a parallel region, each a contiguous part of the
 *        pages, so that the pages of a buffer that has not been written yet are placed on the NUMA nodes of the threads
 *        (a page is placed on the node of the thread that writes it first); the threads are those the calling thread
 *        may run on (see region_affinity); a buffer of fewer pages than threads, or a call inside a parallel region,
 *        leaves the buffer as it is
 * @param data the buffer
 * @param size its size in bytes
 */
inline void place_pages_on_threads(char* data, uint64_t size)
{
    if (omp_in_parallel()) return;
    const uint64_t page = platform::page_size();
    const uint64_t num_threads = std::clamp<uint64_t>(allowed_cpus().count(), 1, std::max(1, omp_get_max_threads()));
    // the first page boundary in the buffer and the end of the buffer
    const uintptr_t first = (uintptr_t(data) + page - 1) & ~uintptr_t(page - 1);
    const uintptr_t end = uintptr_t(data) + size;
    if (first >= end || (end - first) / page < num_threads) return;
    const uint64_t num_pages = (end - first + page - 1) / page;
    region_affinity affinity;

    #pragma omp parallel for num_threads(num_threads) schedule(static) firstprivate(affinity)
    for (uint64_t i = 0; i < num_pages; i++) reinterpret_cast<volatile char*>(first)[i * page] = 0;
}

// the CPUs of a physical core, in increasing order (e.g., its hyper-threads)
using cpu_core_t = std::vector<uint64_t>;

// the cores of a NUMA node, in increasing order of their first CPUs
using cpu_node_t = std::vector<cpu_core_t>;

#if defined(__linux__)
/**
 * @brief reads a list of CPUs in the format of the Linux sysfs, e.g. "0-3,8,10-11"
 * @param path the file storing the list
 * @return the CPUs in the order of the list (empty if the file cannot be read)
 */
inline std::vector<uint64_t> read_cpu_list(const std::string& path)
{
    std::ifstream in(path);
    std::string list;
    std::vector<uint64_t> cpus;
    if (!(in >> list)) return cpus;
    std::istringstream ranges(list);

    for (std::string range; std::getline(ranges, range, ',');) {
        const size_t dash = range.find('-');
        const uint64_t first = std::stoull(range.substr(0, dash));
        const uint64_t last = dash == std::string::npos ? first : std::stoull(range.substr(dash + 1));
        for (uint64_t cpu = first; cpu <= last; cpu++) cpus.push_back(cpu);
    }

    return cpus;
}
#endif

/**
 * @brief returns the NUMA nodes of the machine with their cores, read once from /sys/devices/system/node and the
 *        thread siblings in /sys/devices/system/cpu
 * @return [0..number of nodes - 1] the nodes in increasing order of their numbers; without NUMA information, one node
 *         with all online CPUs; empty if the topology cannot be read (not on Linux)
 */
inline const std::vector<cpu_node_t>& machine_cpu_topology()
{
    static const std::vector<cpu_node_t> nodes = [] {
        std::vector<cpu_node_t> nodes;

#if defined(__linux__)
        namespace fs = std::filesystem;
        const std::string node_prefix = "node";
        // the CPUs of each node, by the number of the node
        std::map<uint64_t, std::vector<uint64_t>> node_cpus;
        std::error_code error;

        for (const fs::directory_entry& entry : fs::directory_iterator("/sys/devices/system/node", error)) {
            const std::string name = entry.path().filename().string();
            if (name.size() <= node_prefix.size() || name.compare(0, node_prefix.size(), node_prefix) != 0) continue;
            const std::string number = name.substr(node_prefix.size());
            if (!std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
            node_cpus[std::stoull(number)] = read_cpu_list(entry.path().string() + "/cpulist");
        }

        if (node_cpus.empty()) node_cpus[0] = read_cpu_list("/sys/devices/system/cpu/online");

        for (const auto& [number, cpus] : node_cpus) {
            // the CPUs of each core, by the first CPU of the core
            std::map<uint64_t, cpu_core_t> cores;

            for (uint64_t cpu : cpus) {
                const std::vector<uint64_t> siblings = read_cpu_list(
                    "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
                cores[siblings.empty() ? cpu : *std::min_element(siblings.begin(), siblings.end())].push_back(cpu);
            }

            if (cores.empty()) continue;
            cpu_node_t& node = nodes.emplace_back();
            for (auto& [first, core] : cores) node.push_back(std::move(core));
        }
#endif

        return nodes;
    }();

    return nodes;
}

/**
 * @brief returns the NUMA nodes with the cores of the CPUs a set allows (see machine_cpu_topology); a node or core
 *        without an allowed CPU is left out
 * @param allowed the CPUs the threads may run on
 * @return the nodes in increasing order of their numbers; empty if the topology cannot be read (not on Linux)
 */
inline std::vector<cpu_node_t> cpu_topology(const platform::cpu_mask& allowed)
{
    std::vector<cpu_node_t> nodes;

    for (const cpu_node_t& machine_node : machine_cpu_topology()) {
        cpu_node_t node;

        for (const cpu_core_t& machine_core : machine_node) {
            cpu_core_t core;
            for (uint64_t cpu : machine_core) if (cpu < allowed.end() && allowed.test(cpu)) core.push_back(cpu);
            if (!core.empty()) node.push_back(std::move(core));
        }

        if (!node.empty()) nodes.push_back(std::move(node));
    }

    return nodes;
}

/**
 * @brief reserves cores of one NUMA node for the threads of a task while in scope, one thread per physical core: on
 *        the node with the most allowed cores (see allowed_cpus) that no other node_cores in scope has reserved, the
 *        cores with the fewest reservations; without topology information (see cpu_topology), all allowed CPUs; the
 *        threads of the task run on cpus() (see full_affinity)
 */
class node_cores {
    platform::cpu_mask _cpus; // one CPU of each reserved core
    std::vector<uint64_t> _reserved; // the CPUs in _cpus, if they are counted in reservations()
    uint64_t _num_threads = 1; // the number of threads of the task

    // returns the number of node_cores in scope that reserved each CPU, by the CPU
    static std::map<uint64_t, uint64_t>& reservations()
    {
        static std::map<uint64_t, uint64_t> num_reservations;
        return num_reservations;
    }

    // returns the lock protecting reservations()
    static std::mutex& reservations_lock()
    {
        static std::mutex lock;
        return lock;
    }

public:
    /**
     * @brief reserves cores
     * @param max_threads the maximum number of threads of the task (at least one core is reserved)
     */
    explicit node_cores(uint64_t max_threads)
    {
        const platform::cpu_mask& allowed = allowed_cpus();
        const std::vector<cpu_node_t> nodes = cpu_topology(allowed);

        if (nodes.empty()) {
            _cpus = allowed;
            _num_threads = std::clamp<uint64_t>(max_threads, 1, std::max<uint64_t>(1, allowed.count()));
            return;
        }

        std::lock_guard<std::mutex> guard(reservations_lock());
        std::map<uint64_t, uint64_t>& reserved = reservations();

        // returns the number of cores of a node that no node_cores in scope has reserved
        auto num_free_cores = [&](const cpu_node_t& node) {
            return uint64_t(std::count_if(node.begin(), node.end(),
                [&](const cpu_core_t& core) { return reserved[core.front()] == 0; }));
        };

        const cpu_node_t* node = &nodes.front();
        uint64_t free_cores = num_free_cores(*node);

        for (const cpu_node_t& other : nodes) {
            const uint64_t other_free_cores = num_free_cores(other);

            if (other_free_cores > free_cores) {
                node = &other;
                free_cores = other_free_cores;
            }
        }

        // the cores of the node by increasing number of reservations
        std::vector<uint64_t> cpus;
        for (const cpu_core_t& core : *node) cpus.push_back(core.front());
        std::stable_sort(cpus.begin(), cpus.end(), [&](uint64_t x, uint64_t y) { return reserved[x] < reserved[y]; });
        _num_threads = std::clamp<uint64_t>(max_threads, 1, cpus.size());
        _reserved.assign(cpus.begin(), cpus.begin() + _num_threads);

        for (uint64_t cpu : _reserved) {
            _cpus.set(cpu);
            reserved[cpu]++;
        }
    }

    node_cores(const node_cores&) = delete;
    node_cores& operator=(const node_cores&) = delete;

    ~node_cores()
    {
        if (_reserved.empty()) return;
        std::lock_guard<std::mutex> guard(reservations_lock());
        for (uint64_t cpu : _reserved) reservations()[cpu]--;
    }

    // returns the CPUs the threads of the task run on
    const platform::cpu_mask& cpus() const { return _cpus; }

    // returns the number of threads of the task: one per reserved core, or at most one per allowed CPU
    uint64_t num_threads() const { return _num_threads; }
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
