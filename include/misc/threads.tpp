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

#ifndef STRING_UTILS_MISC_THREADS_TPP
#define STRING_UTILS_MISC_THREADS_TPP

#include <misc/threads.hpp>

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

inline node_cores::node_cores(uint64_t max_threads)
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
    std::vector<const cpu_core_t*> cores;
    for (const cpu_core_t& core : *node) cores.push_back(&core);
    std::stable_sort(cores.begin(), cores.end(),
        [&](const cpu_core_t* x, const cpu_core_t* y) { return reserved[x->front()] < reserved[y->front()]; });
    _num_threads = std::clamp<uint64_t>(max_threads, 1, cores.size());

    // the k-th reservation of a core runs on its k-th CPU (modulo the number of its CPUs), so that tasks sharing a
    // core use its hardware threads
    for (uint64_t i = 0; i < _num_threads; i++) {
        const uint64_t first = cores[i]->front();
        cpu_core_t core = *cores[i];
        std::rotate(core.begin(), core.begin() + reserved[first] % core.size(), core.end());
        _reserved.push_back(first);
        _cpus.set(core.front());
        _cores.push_back(std::move(core));
        reserved[first]++;
    }
}

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

#endif // STRING_UTILS_MISC_THREADS_TPP
