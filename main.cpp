#include "scheduler_sim.h"
#include <iostream>
#include <memory>
#include <vector>

int main() {
    const int seed = 2026;
    const int horizon = 4000;
    std::vector<Core> cores = {
        {"P", 1.8, 0, 0},
        {"P", 1.8, 0, 0},
        {"E", 0.7, 0, 0},
        {"E", 0.7, 0, 0}
    };

    std::vector<std::unique_ptr<Scheduler>> schedulers = create_schedulers();

    std::vector<Metrics> results;
    for (auto& scheduler : schedulers) {
        Metrics result = run_simulation(*scheduler, make_workload(seed), cores, horizon);
        results.push_back(result);
        std::cout << result.scheduler << ": "
                  << "response=" << result.average_response << " ms, "
                  << "turnaround=" << result.average_turnaround << " ms, "
                  << "util=" << result.cpu_utilization << "%, "
                  << "fairness=" << result.jain_fairness << '\n';
    }

    write_csv("scheduler_results.csv", results);
    return 0;
}