#include "include/scheduler_sim.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>

namespace {

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
    index = std::min(index, values.size() - 1);
    return values[index];
}

} // namespace

std::vector<Task> make_workload(int seed) {
    std::mt19937 rng(seed);
    std::vector<Task> tasks;
    tasks.reserve(40);

    for (int i = 0; i < 40; ++i) {
        Task task;
        task.id = i;
        task.arrival = static_cast<int>(rng() % 201);
        task.burst = 10 + static_cast<int>(rng() % 491);
        task.remaining = task.burst;
        task.priority = 1 + static_cast<int>(rng() % 10);
        task.interactivity = static_cast<int>(rng() % 101);
        task.foreground = i < 12 || task.interactivity >= 75;
        tasks.push_back(task);
    }
    return tasks;
}

Metrics run_simulation(Scheduler& scheduler,
                       std::vector<Task> tasks,
                       const std::vector<Core>& core_template,
                       int horizon) {
    std::vector<Core> cores = core_template;
    std::vector<Task*> running(cores.size(), nullptr);
    std::set<int> arrived;
    int context_switches = 0;

    for (int time = 0; time < horizon; ++time) {
        for (Task& task : tasks) {
            if (task.arrival == time) {
                arrived.insert(task.id);
                double weight = 1024.0 / std::pow(1.25, task.priority - 1);
                task.virtual_deadline = task.vruntime + 6.0 * (1024.0 / weight);
                scheduler.enqueue(&task, time);
            }
        }

        for (size_t c = 0; c < cores.size(); ++c) {
            Core& core = cores[c];

            if (running[c] == nullptr) {
                Task* next = scheduler.pick_next(core, time);
                if (next != nullptr) {
                    running[c] = next;
                    if (next->start == -1) next->start = time;
                    ++context_switches;
                }
            }

            Task* task = running[c];
            if (task == nullptr) continue;

            task->remaining -= 1;
            core.busy_ticks++;
            task->vruntime += 1.0 / (1024.0 / std::pow(1.25, task->priority - 1));

            if (task->remaining <= 0) {
                task->completion = time + 1;
                running[c] = nullptr;
                ++context_switches;
                continue;
            }

            --core.slice_left;
            if (core.slice_left <= 0) {
                double weight = 1024.0 / std::pow(1.25, task->priority - 1);
                task->virtual_deadline = task->vruntime + 6.0 * (1024.0 / weight);
                scheduler.enqueue(task, time);
                running[c] = nullptr;
                ++context_switches;
            }
        }
    }

    std::vector<double> responses;
    std::vector<double> turnarounds;
    double fairness_numerator = 0.0;
    double fairness_denominator = 0.0;
    int completed = 0;
    for (const Task& task : tasks) {
        if (task.completion >= 0) {
            ++completed;
            responses.push_back(task.start - task.arrival);
            turnarounds.push_back(task.completion - task.arrival);
            double share = task.completion >= 0
                ? static_cast<double>(task.burst) / (task.completion - task.arrival)
                : 0.0;
            fairness_numerator += share;
            fairness_denominator += share * share;
        }
    }

    double avg_response = 0.0;
    double avg_turnaround = 0.0;
    for (double value : responses) avg_response += value;
    for (double value : turnarounds) avg_turnaround += value;
    if (!responses.empty()) avg_response /= responses.size();
    if (!turnarounds.empty()) avg_turnaround /= turnarounds.size();

    int busy_ticks = 0;
    double energy = 0.0;
    for (const Core& core : cores) {
        busy_ticks += core.busy_ticks;
        energy += core.busy_ticks * (core.type == "P" ? 1.8 : 0.7);
    }
    double util = cores.empty()
        ? 0.0
        : static_cast<double>(busy_ticks) / (horizon * cores.size()) * 100.0;
    double fairness = fairness_denominator > 0.0
        ? (fairness_numerator * fairness_numerator)
          / (static_cast<double>(completed) * fairness_denominator)
        : 0.0;

    Metrics metrics;
    metrics.scheduler = scheduler.name();
    metrics.total_ticks = busy_ticks > 0 ? horizon : 0;
    metrics.average_response = avg_response;
    metrics.p95_response = percentile(responses, 0.95);
    metrics.average_turnaround = avg_turnaround;
    metrics.cpu_utilization = util;
    metrics.context_switches = static_cast<double>(context_switches);
    metrics.jain_fairness = fairness;
    metrics.energy = energy;
    return metrics;
}

void write_csv(const std::string& path, const std::vector<Metrics>& results) {
    std::ofstream file(path);
    file << "Scheduler,TotalTime,AvgResponse,P95Latency,AvgTurnaround,"
            "CPUUtil,CSOverhead,JainFairness,Energy\n";
    file << std::fixed << std::setprecision(3);
    for (const Metrics& metric : results) {
        file << metric.scheduler << ','
             << metric.total_ticks << ','
             << metric.average_response << ','
             << metric.p95_response << ','
             << metric.average_turnaround << ','
             << metric.cpu_utilization << ','
             << metric.context_switches << ','
             << metric.jain_fairness << ','
             << metric.energy << '\n';
    }
}
