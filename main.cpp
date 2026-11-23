#include "include/scheduler_sim.h"
#include <iostream>

int main() {
    const unsigned int seed = 2026;
    const int horizon = 4000;
    const int task_count = MAX_TASKS;
    const int core_count = MAX_CORES;
    const Core cores[MAX_CORES] = {
        {'P', 1.8, 2.8, 0, 0},
        {'P', 1.8, 2.8, 0, 0},
        {'E', 0.7, 0.65, 0, 0},
        {'E', 0.7, 0.65, 0, 0}
    };

    RoundRobinScheduler round_robin;
    LinuxEEVDFScheduler linux_eevdf;
    FreeBSDULEScheduler freebsd_ule;
    AndroidEASScheduler android_eas;
    Scheduler* schedulers[SCHEDULER_COUNT] = {
        &round_robin,
        &linux_eevdf,
        &freebsd_ule,
        &android_eas
    };

    print_table_header();
    print_csv_header();
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        Task tasks[MAX_TASKS];
        make_workload(seed, tasks, task_count);
        Metrics result = run_simulation(
            *schedulers[i], tasks, task_count, cores, core_count, horizon);
        print_table_row(result);
        print_csv_row(result);
    }
    std::cout << "\n* Jain compares actual work with estimated priority-weighted service.\n"
                 "  Switches count dispatches after each core's first dispatch; "
                 "switch time is not charged.\n";
    return 0;
}