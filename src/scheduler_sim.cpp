#include "scheduler_sim.h"
#include <iostream>

namespace {

unsigned int next_random(unsigned int& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

double priority_factor(int priority) {
    double factor = 1.0;
    for (int i = 1; i < priority; ++i) {
        factor *= 1.25;
    }
    return factor;
}

double response_percentile(int responses[], int count) {
    if (count == 0) {
        return 0.0;
    }

    for (int i = 1; i < count; ++i) {
        int value = responses[i];
        int j = i - 1;
        while (j >= 0 && responses[j] > value) {
            responses[j + 1] = responses[j];
            --j;
        }
        responses[j + 1] = value;
    }

    int index = (95 * count + 99) / 100 - 1;
    return responses[index];
}

} // namespace

void make_workload(unsigned int seed, Task tasks[], int task_count) {
    unsigned int state = seed;
    if (task_count > MAX_TASKS) {
        task_count = MAX_TASKS;
    }

    for (int i = 0; i < task_count; ++i) {
        tasks[i].id = i;
        tasks[i].arrival = static_cast<int>(next_random(state) % 201u);
        tasks[i].burst = 10 + static_cast<int>(next_random(state) % 491u);
        tasks[i].remaining = tasks[i].burst;
        tasks[i].priority = 1 + static_cast<int>(next_random(state) % 10u);
        tasks[i].interactivity = static_cast<int>(next_random(state) % 101u);
        tasks[i].foreground = i < 12 || tasks[i].interactivity >= 75;
        tasks[i].vruntime = 0.0;
        tasks[i].virtual_deadline = 0.0;
        tasks[i].start = -1;
        tasks[i].completion = -1;
    }
}

Metrics run_simulation(Scheduler& scheduler,
                       Task tasks[],
                       int task_count,
                       const Core core_template[],
                       int core_count,
                       int horizon) {
    if (task_count > MAX_TASKS) {
        task_count = MAX_TASKS;
    }
    if (core_count > MAX_CORES) {
        core_count = MAX_CORES;
    }

    Core cores[MAX_CORES];
    Task* running[MAX_CORES];
    for (int c = 0; c < core_count; ++c) {
        cores[c] = core_template[c];
        cores[c].busy_ticks = 0;
        cores[c].slice_left = 0;
        running[c] = 0;
    }

    int context_switches = 0;
    for (int time = 0; time < horizon; ++time) {
        for (int i = 0; i < task_count; ++i) {
            Task& task = tasks[i];
            if (task.arrival == time) {
                double factor = priority_factor(task.priority);
                task.virtual_deadline = task.vruntime + 6.0 * factor;
                scheduler.enqueue(&task, time);
            }
        }

        for (int c = 0; c < core_count; ++c) {
            Core& core = cores[c];
            if (running[c] == 0) {
                Task* next = scheduler.pick_next(core, time);
                if (next != 0) {
                    running[c] = next;
                    if (next->start == -1) {
                        next->start = time;
                    }
                    ++context_switches;
                }
            }

            Task* task = running[c];
            if (task == 0) {
                continue;
            }

            --task->remaining;
            ++core.busy_ticks;
            task->vruntime += priority_factor(task->priority) / 1024.0;

            if (task->remaining <= 0) {
                task->completion = time + 1;
                running[c] = 0;
                ++context_switches;
                continue;
            }

            --core.slice_left;
            if (core.slice_left <= 0) {
                task->virtual_deadline =
                    task->vruntime + 6.0 * priority_factor(task->priority);
                scheduler.enqueue(task, time);
                running[c] = 0;
                ++context_switches;
            }
        }
    }

    int responses[MAX_TASKS];
    int response_count = 0;
    int completed = 0;
    double response_sum = 0.0;
    double turnaround_sum = 0.0;
    double fairness_sum = 0.0;
    double fairness_square_sum = 0.0;

    for (int i = 0; i < task_count; ++i) {
        const Task& task = tasks[i];
        if (task.completion >= 0) {
            ++completed;
            int response = task.start - task.arrival;
            int turnaround = task.completion - task.arrival;
            responses[response_count++] = response;
            response_sum += response;
            turnaround_sum += turnaround;

            double share = static_cast<double>(task.burst) / turnaround;
            fairness_sum += share;
            fairness_square_sum += share * share;
        }
    }

    int busy_ticks = 0;
    double energy = 0.0;
    for (int c = 0; c < core_count; ++c) {
        busy_ticks += cores[c].busy_ticks;
        energy += cores[c].busy_ticks * (cores[c].type == 'P' ? 1.8 : 0.7);
    }

    Metrics metrics;
    metrics.scheduler = scheduler.name();
    metrics.total_ticks = busy_ticks > 0 ? horizon : 0;
    metrics.average_response = completed > 0 ? response_sum / completed : 0.0;
    metrics.p95_response = response_percentile(responses, response_count);
    metrics.average_turnaround =
        completed > 0 ? turnaround_sum / completed : 0.0;
    metrics.cpu_utilization = core_count > 0 && horizon > 0
        ? static_cast<double>(busy_ticks) / (horizon * core_count) * 100.0
        : 0.0;
    metrics.context_switches = context_switches;
    metrics.jain_fairness = fairness_square_sum > 0.0
        ? fairness_sum * fairness_sum / (completed * fairness_square_sum)
        : 0.0;
    metrics.energy = energy;
    return metrics;
}

void print_csv_header() {
    std::cout << "Scheduler,TotalTime,AvgResponse,P95Latency,AvgTurnaround,"
                 "CPUUtil,CSOverhead,JainFairness,Energy\n";
}

void print_csv_row(const Metrics& metrics) {
    std::cout << metrics.scheduler << ','
              << metrics.total_ticks << ','
              << metrics.average_response << ','
              << metrics.p95_response << ','
              << metrics.average_turnaround << ','
              << metrics.cpu_utilization << ','
              << metrics.context_switches << ','
              << metrics.jain_fairness << ','
              << metrics.energy << '\n';
}
