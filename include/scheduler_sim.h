#ifndef SCHEDULER_SIM_H
#define SCHEDULER_SIM_H

#include <deque>
#include <memory>
#include <string>
#include <vector>

struct Task {
    int id;
    int arrival;
    int burst;
    int remaining;
    int priority;
    int interactivity;
    bool foreground;
    double vruntime = 0.0;
    double virtual_deadline = 0.0;
    int start = -1;
    int completion = -1;
};

struct Core {
    std::string type;
    double speed;
    int slice_left = 0;
    int busy_ticks = 0;
};

struct Metrics {
    std::string scheduler;
    int total_ticks = 0;
    double average_response = 0.0;
    double p95_response = 0.0;
    double average_turnaround = 0.0;
    double cpu_utilization = 0.0;
    double context_switches = 0.0;
    double jain_fairness = 0.0;
    double energy = 0.0;
};

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual std::string name() const = 0;
    virtual void enqueue(Task* task, int time) = 0;
    virtual Task* pick_next(Core& core, int time) = 0;
};

std::vector<Task> make_workload(int seed);
std::vector<std::unique_ptr<Scheduler>> create_schedulers();
Metrics run_simulation(Scheduler& scheduler,
                       std::vector<Task> tasks,
                       const std::vector<Core>& core_template,
                       int horizon);
void write_csv(const std::string& path, const std::vector<Metrics>& results);

#endif
