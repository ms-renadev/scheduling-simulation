#include <iostream>
#include <random>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <cmath>
#include <string>

const int MAX_TASKS = 250;
const int MAX_QUEUE = 1000;
const int MAX_BURSTS = 15;

// ============================================================================
// 1. PURE ARRAY DATA STRUCTURES (Walang STL vector/deque/queue)
// ============================================================================
struct Task {
    int pid;
    std::string type;          // "SPOTIFY", "MESSENGER", "WEB", "BATCH"
    int arrival_time;
    int bursts[MAX_BURSTS];    // Fixed array imbes na std::vector
    int num_bursts = 0;
    int priority;              // 1 (Urgent) to 10 (Background)

    int burst_idx = 0;
    int remaining_burst = 0;
    int io_wake_time = 0;
    int first_run_time = -1;
    int completion_time = -1;
    bool is_blocked = false;

    // Scheduler-specific fields
    double vruntime = 0.0;
    double virtual_deadline = 0.0;
    int total_cpu = 0;
    int total_sleep = 0;
};

struct Core {
    int id;
    std::string type;          // "P" (Performance) or "E" (Efficiency)
    int speed;                 // 2 for P-core, 1 for E-core
    double power_cost;         // 4.0 for P-core, 1.0 for E-core
    Task* current_task = nullptr;
    int slice_left = 0;
    int switch_cooldown = 0;
};

struct MetricsResult {
    std::string name;
    int total_time;
    double avg_ui_resp;
    int p95_ui_latency;
    double avg_turnaround;
    double cpu_util;
    double cs_overhead;
    double jain_fairness;
    int total_energy;
};

// Simpleng Array-Based FIFO Queue (Pamalit sa std::deque)
struct ArrayQueue {
    Task* items[MAX_QUEUE];
    int head = 0;
    int tail = 0;
    int count = 0;

    bool empty() const { return count == 0; }
    int size() const { return count; }

    void push(Task* t) {
        if (count >= MAX_QUEUE) return;
        items[tail] = t;
        tail = (tail + 1) % MAX_QUEUE;
        count++;
    }

    Task* pop() {
        if (empty()) return nullptr;
        Task* t = items[head];
        head = (head + 1) % MAX_QUEUE;
        count--;
        return t;
    }
};

// ============================================================================
// 2. WORKLOAD GENERATOR (Pure Array Version)
// ============================================================================
void generateWorkload(Task tasks[], int count, int seed = 101) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    int current_arrival = 0;

    for (int i = 0; i < count; ++i) {
        current_arrival += rng() % 6;
        double roll = dist(rng);
        Task t;
        t.pid = i + 1;
        t.arrival_time = current_arrival;
        t.num_bursts = 0;

        if (roll < 0.30) {
            t.type = "SPOTIFY";
            t.priority = 1;
            for (int b = 0; b < 6; ++b) {
                t.bursts[t.num_bursts++] = 2;
                if (b < 5) t.bursts[t.num_bursts++] = 20;
            }
        } else if (roll < 0.65) {
            t.type = "MESSENGER";
            t.priority = 2 + (rng() % 2);
            int n = 4 + (rng() % 4);
            for (int b = 0; b < n; ++b) {
                t.bursts[t.num_bursts++] = 2 + (rng() % 4);
                if (b < n - 1) t.bursts[t.num_bursts++] = 12 + (rng() % 19);
            }
        } else if (roll < 0.88) {
            t.type = "WEB";
            t.priority = 4 + (rng() % 3);
            t.bursts[0] = 10 + (rng() % 13);
            t.bursts[1] = 20 + (rng() % 21);
            t.bursts[2] = 10 + (rng() % 13);
            t.num_bursts = 3;
        } else {
            t.type = "BATCH";
            t.priority = 8 + (rng() % 3);
            t.bursts[0] = 70 + (rng() % 71);
            t.bursts[1] = 10 + (rng() % 11);
            t.bursts[2] = 70 + (rng() % 71);
            t.num_bursts = 3;
        }
        t.remaining_burst = t.bursts[0];
        tasks[i] = t;
    }
}

// ============================================================================
// 3. BASE SCHEDULER & ALGORITHMS (Pure Arrays)
// ============================================================================
class BaseScheduler {
public:
    virtual std::string getName() = 0;
    virtual void enqueue(Task* t, int time) = 0;
    virtual Task* pickNext(Core& core, int time) = 0;
    virtual ~BaseScheduler() = default;
};

// --- 1. ROUND ROBIN (BASELINE) ---
class RoundRobinScheduler : public BaseScheduler {
    ArrayQueue q;
public:
    std::string getName() override { return "1. Round Robin (Baseline)"; }
    void enqueue(Task* t, int time) override { q.push(t); }
    Task* pickNext(Core& core, int time) override {
        if (q.empty()) return nullptr;
        core.slice_left = 8;
        return q.pop();
    }
};

// --- 2. LINUX EEVDF (Array Min-Search imbes na priority_queue) ---
class LinuxEEVDFScheduler : public BaseScheduler {
    Task* ready[MAX_QUEUE];
    int count = 0;

public:
    std::string getName() override { return "2. Linux (EEVDF)"; }

    void enqueue(Task* t, int time) override {
        if (count >= MAX_QUEUE) return;
        double weight = 1024.0 / std::pow(1.25, t->priority - 1);
        t->virtual_deadline = t->vruntime + (6.0 * (1024.0 / weight));
        ready[count++] = t;
    }

    Task* pickNext(Core& core, int time) override {
        if (count == 0) return nullptr;
        int best_idx = 0;
        for (int i = 1; i < count; ++i) {
            if (ready[i]->virtual_deadline < ready[best_idx]->virtual_deadline) {
                best_idx = i;
            }
        }
        Task* best = ready[best_idx];
        ready[best_idx] = ready[--count];
        core.slice_left = 6;
        return best;
    }
};

// --- 3. FREEBSD ULE (Gamit ang ArrayQueue) ---
class FreeBSDULEScheduler : public BaseScheduler {
    ArrayQueue interactive_q;
    ArrayQueue batch_q;

public:
    std::string getName() override { return "3. FreeBSD (ULE)"; }

    void enqueue(Task* t, int time) override {
        if (t->total_sleep >= t->total_cpu || t->priority <= 3) {
            interactive_q.push(t);
        } else {
            batch_q.push(t);
        }
    }

    Task* pickNext(Core& core, int time) override {
        if (!interactive_q.empty()) {
            core.slice_left = 4;
            return interactive_q.pop();
        }
        if (!batch_q.empty()) {
            core.slice_left = 10;
            return batch_q.pop();
        }
        return nullptr;
    }
};

// --- 4. ANDROID EAS + UCLAMP (Gamit ang ArrayQueue) ---
class AndroidEASScheduler : public BaseScheduler {
    ArrayQueue perf_q;
    ArrayQueue eff_q;

public:
    std::string getName() override { return "4. Android (EAS + uclamp)"; }

    void enqueue(Task* t, int time) override {
        if (t->priority <= 4) {
            perf_q.push(t);
        } else {
            eff_q.push(t);
        }
    }

    Task* pickNext(Core& core, int time) override {
        if (core.type == "P") {
            if (!perf_q.empty()) {
                core.slice_left = 5;
                return perf_q.pop();
            } else if (eff_q.size() > 6) {
                core.slice_left = 8;
                return eff_q.pop();
            }
        } else if (core.type == "E") {
            if (!eff_q.empty()) {
                core.slice_left = 10;
                return eff_q.pop();
            } else if (!perf_q.empty()) {
                core.slice_left = 5;
                return perf_q.pop();
            }
        }
        return nullptr;
    }
};

// ============================================================================
// 4. SIMULATION ENGINE & METRICS (Pure Array Version)
// ============================================================================
MetricsResult runSimulation(BaseScheduler& sched, const Task base_tasks[], int total_tasks) {
    Task tasks[MAX_TASKS];
    for (int i = 0; i < total_tasks; ++i) tasks[i] = base_tasks[i];

    Core cores[4] = {
        {0, "P", 2, 4.0}, {1, "P", 2, 4.0},
        {2, "E", 1, 1.0}, {3, "E", 1, 1.0}
    };

    int time = 0, completed = 0, next_arrival_idx = 0;
    int active_ticks = 0, cs_ticks = 0;
    double total_energy = 0.0;

    while (completed < total_tasks) {
        while (next_arrival_idx < total_tasks && tasks[next_arrival_idx].arrival_time <= time) {
            sched.enqueue(&tasks[next_arrival_idx++], time);
        }

        // Check blocked tasks using a simple array scan
        for (int i = 0; i < next_arrival_idx; ++i) {
            if (tasks[i].is_blocked) {
                if (tasks[i].io_wake_time <= time) {
                    tasks[i].is_blocked = false;
                    sched.enqueue(&tasks[i], time);
                } else {
                    tasks[i].total_sleep++;
                }
            }
        }

        for (int c = 0; c < 4; ++c) {
            Core& core = cores[c];
            if (core.switch_cooldown > 0) {
                core.switch_cooldown--;
                cs_ticks++;
                total_energy += core.power_cost * 0.5;
                continue;
            }
            if (!core.current_task) {
                Task* next_t = sched.pickNext(core, time);
                if (next_t) {
                    core.current_task = next_t;
                    core.switch_cooldown = 1;
                    continue;
                }
            }
            if (Task* t = core.current_task) {
                if (t->first_run_time == -1) t->first_run_time = time;
                int work = std::min(t->remaining_burst, core.speed);
                t->remaining_burst -= work;
                t->total_cpu++;
                double weight = 1024.0 / std::pow(1.25, t->priority - 1);
                t->vruntime += (1024.0 / weight) * ((double)work / core.speed);

                core.slice_left--;
                active_ticks++;
                total_energy += core.power_cost;

                if (t->remaining_burst <= 0) {
                    t->burst_idx++;
                    core.current_task = nullptr;
                    if (t->burst_idx >= t->num_bursts) {
                        t->completion_time = time + 1;
                        completed++;
                    } else {
                        int io_dur = t->bursts[t->burst_idx++];
                        t->remaining_burst = t->bursts[t->burst_idx];
                        t->io_wake_time = time + 1 + io_dur;
                        t->is_blocked = true;
                    }
                } else if (core.slice_left <= 0) {
                    core.current_task = nullptr;
                    sched.enqueue(t, time + 1);
                }
            }
        }
        time++;
        if (time > 100000) break;
    }

    int ui_resps[MAX_TASKS];
    int ui_count = 0, batch_count = 0;
    double sum_turnaround = 0.0, sum_ratio = 0.0, sum_ratio_sq = 0.0;
    for (int i = 0; i < total_tasks; ++i) {
        if (tasks[i].completion_time == -1) continue;
        int turnaround = tasks[i].completion_time - tasks[i].arrival_time;
        sum_turnaround += turnaround;
        if (tasks[i].type == "SPOTIFY" || tasks[i].type == "MESSENGER") {
            ui_resps[ui_count++] = tasks[i].first_run_time - tasks[i].arrival_time;
        }
        if (tasks[i].type == "BATCH") {
            int cpu_work = 0;
            for (int b = 0; b < tasks[i].num_bursts; b += 2) cpu_work += tasks[i].bursts[b];
            double ratio = (double)cpu_work / std::max(1, turnaround);
            sum_ratio += ratio;
            sum_ratio_sq += ratio * ratio;
            batch_count++;
        }
    }

    std::sort(ui_resps, ui_resps + ui_count);
    double avg_ui = 0;
    for (int i = 0; i < ui_count; ++i) avg_ui += ui_resps[i];
    avg_ui = (ui_count > 0) ? avg_ui / ui_count : 0;
    int p95 = (ui_count > 0) ? ui_resps[(int)(0.95 * (ui_count - 1))] : 0;
    double jain = (batch_count > 0 && sum_ratio_sq > 0) ? (sum_ratio * sum_ratio) / (batch_count * sum_ratio_sq) : 0.0;
    double total_cap = time * 4.0;
    return {
        sched.getName(), time, avg_ui, p95,
        sum_turnaround / std::max(1, completed),
        (active_ticks / total_cap) * 100.0,
        (cs_ticks / total_cap) * 100.0,
        jain, (int)total_energy
    };
}

int main() {
    Task workload[MAX_TASKS];
    generateWorkload(workload, MAX_TASKS, 101);

    RoundRobinScheduler rr;
    LinuxEEVDFScheduler linux_sched;
    FreeBSDULEScheduler bsd_sched;
    AndroidEASScheduler android_sched;

    MetricsResult results[4] = {
        runSimulation(rr, workload, MAX_TASKS),
        runSimulation(linux_sched, workload, MAX_TASKS),
        runSimulation(bsd_sched, workload, MAX_TASKS),
        runSimulation(android_sched, workload, MAX_TASKS)
    };

    std::ofstream csv("scheduler_results.csv");
    csv << "Scheduler,TotalTime,AvgUIResp,P95Latency,AvgTurnaround,CPUUtil,CSOverhead,JainFairness,Energy\n";

    std::cout << std::left << std::setw(28) << "Scheduler"
              << std::setw(10) << "Time(ms)"
              << std::setw(12) << "UI_Resp"
              << std::setw(10) << "P95(ms)"
              << std::setw(13) << "Turnaround"
              << std::setw(10) << "CPU(%)"
              << std::setw(10) << "CS(%)"
              << std::setw(10) << "Fairness"
              << "Energy\n" << std::string(105, '-') << "\n";

    for (int i = 0; i < 4; ++i) {
        const auto& r = results[i];
        std::cout << std::left << std::setw(28) << r.name
                  << std::setw(10) << r.total_time
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.avg_ui_resp
                  << std::setw(10) << r.p95_ui_latency
                  << std::setw(13) << std::setprecision(1) << r.avg_turnaround
                  << std::setw(10) << r.cpu_util
                  << std::setw(10) << r.cs_overhead
                  << std::setw(10) << std::setprecision(3) << r.jain_fairness
                  << r.total_energy << "\n";

        csv << r.name << "," << r.total_time << "," << r.avg_ui_resp << ","
            << r.p95_ui_latency << "," << r.avg_turnaround << "," << r.cpu_util << ","
            << r.cs_overhead << "," << r.jain_fairness << "," << r.total_energy << "\n";
    }

    std::cout << "\nSaved to scheduler_results.csv!\n";
    return 0;
}