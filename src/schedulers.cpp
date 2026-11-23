#include "include/scheduler_sim.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>

namespace {

class RoundRobin : public Scheduler {
    std::deque<Task*> ready;
public:
    std::string name() const override { return "Round Robin"; }
    void enqueue(Task* task, int) override { ready.push_back(task); }
    Task* pick_next(Core& core, int) override {
        if (ready.empty()) return nullptr;
        core.slice_left = 10;
        Task* task = ready.front();
        ready.pop_front();
        return task;
    }
};

class LinuxEEVDF : public Scheduler {
    std::vector<Task*> ready;
public:
    std::string name() const override { return "Linux EEVDF"; }
    void enqueue(Task* task, int) override { ready.push_back(task); }
    Task* pick_next(Core& core, int) override {
        if (ready.empty()) return nullptr;
        auto selected = std::min_element(ready.begin(), ready.end(),
            [](const Task* a, const Task* b) {
                return a->virtual_deadline < b->virtual_deadline;
            });
        Task* task = *selected;
        ready.erase(selected);
        core.slice_left = 6;
        return task;
    }
};

class FreeBSDULE : public Scheduler {
    std::deque<Task*> interactive;
    std::deque<Task*> batch;
public:
    std::string name() const override { return "FreeBSD ULE"; }
    void enqueue(Task* task, int) override {
        if (task->foreground || task->interactivity >= 60)
            interactive.push_back(task);
        else
            batch.push_back(task);
    }
    Task* pick_next(Core& core, int) override {
        if (!interactive.empty()) {
            core.slice_left = 4;
            Task* task = interactive.front();
            interactive.pop_front();
            return task;
        }
        if (!batch.empty()) {
            core.slice_left = 10;
            Task* task = batch.front();
            batch.pop_front();
            return task;
        }
        return nullptr;
    }
};

class AndroidEAS : public Scheduler {
    std::deque<Task*> performance;
    std::deque<Task*> efficiency;
public:
    std::string name() const override { return "Android EAS + uclamp"; }
    void enqueue(Task* task, int) override {
        if (task->foreground || task->priority <= 4)
            performance.push_back(task);
        else
            efficiency.push_back(task);
    }
    Task* pick_next(Core& core, int) override {
        if (core.type == "P") {
            if (!performance.empty()) {
                core.slice_left = 5;
                Task* task = performance.front();
                performance.pop_front();
                return task;
            }
            if (!efficiency.empty()) {
                core.slice_left = 8;
                Task* task = efficiency.front();
                efficiency.pop_front();
                return task;
            }
        } else {
            if (!efficiency.empty()) {
                core.slice_left = 10;
                Task* task = efficiency.front();
                efficiency.pop_front();
                return task;
            }
            if (!performance.empty()) {
                core.slice_left = 5;
                Task* task = performance.front();
                performance.pop_front();
                return task;
            }
        }
        return nullptr;
    }
};

} // namespace

std::vector<std::unique_ptr<Scheduler>> create_schedulers() {
    std::vector<std::unique_ptr<Scheduler>> schedulers;
    schedulers.push_back(std::make_unique<RoundRobin>());
    schedulers.push_back(std::make_unique<LinuxEEVDF>());
    schedulers.push_back(std::make_unique<FreeBSDULE>());
    schedulers.push_back(std::make_unique<AndroidEAS>());
    return schedulers;
}
