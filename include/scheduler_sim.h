#ifndef SCHEDULER_SIM_H
#define SCHEDULER_SIM_H

const int MAX_TASKS = 40;
const int MAX_CORES = 4;
const int SCHEDULER_COUNT = 4;

struct Task {
    int id = 0;
    int arrival = 0;
    int burst = 0;
    int remaining = 0;
    int priority = 0;
    int interactivity = 0;
    bool foreground = false;
    double vruntime = 0.0;
    double virtual_deadline = 0.0;
    int start = -1;
    int completion = -1;
};

struct Core {
    char type = 'E';
    double speed = 1.0;
    int slice_left = 0;
    int busy_ticks = 0;
};

struct Metrics {
    const char* scheduler = "";
    int total_ticks = 0;
    double average_response = 0.0;
    double p95_response = 0.0;
    double average_turnaround = 0.0;
    double cpu_utilization = 0.0;
    int context_switches = 0;
    double jain_fairness = 0.0;
    double energy = 0.0;
};

class Scheduler {
public:
    virtual const char* name() const = 0;
    virtual void enqueue(Task* task, int time) = 0;
    virtual Task* pick_next(Core& core, int time) = 0;
};

class RoundRobinScheduler : public Scheduler {
    Task* ready[MAX_TASKS];
    int count;

public:
    RoundRobinScheduler();
    const char* name() const override;
    void enqueue(Task* task, int time) override;
    Task* pick_next(Core& core, int time) override;
};

class LinuxEEVDFScheduler : public Scheduler {
    Task* ready[MAX_TASKS];
    int count;

public:
    LinuxEEVDFScheduler();
    const char* name() const override;
    void enqueue(Task* task, int time) override;
    Task* pick_next(Core& core, int time) override;
};

class FreeBSDULEScheduler : public Scheduler {
    Task* interactive[MAX_TASKS];
    int interactive_count;
    Task* batch[MAX_TASKS];
    int batch_count;

public:
    FreeBSDULEScheduler();
    const char* name() const override;
    void enqueue(Task* task, int time) override;
    Task* pick_next(Core& core, int time) override;
};

class AndroidEASScheduler : public Scheduler {
    Task* performance[MAX_TASKS];
    int performance_count;
    Task* efficiency[MAX_TASKS];
    int efficiency_count;

public:
    AndroidEASScheduler();
    const char* name() const override;
    void enqueue(Task* task, int time) override;
    Task* pick_next(Core& core, int time) override;
};

void make_workload(unsigned int seed, Task tasks[], int task_count);
Metrics run_simulation(Scheduler& scheduler,
                       Task tasks[],
                       int task_count,
                       const Core core_template[],
                       int core_count,
                       int horizon);
void print_csv_header();
void print_csv_row(const Metrics& metrics);

#endif
