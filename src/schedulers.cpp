#include "scheduler_sim.h"

namespace {

void push_back(Task* queue[], int& count, Task* task) {
    if (count < MAX_TASKS) {
        queue[count] = task;
        ++count;
    }
}

Task* pop_front(Task* queue[], int& count) {
    if (count == 0) {
        return 0;
    }

    Task* task = queue[0];
    for (int i = 1; i < count; ++i) {
        queue[i - 1] = queue[i];
    }
    --count;
    return task;
}

Task* remove_at(Task* queue[], int& count, int index) {
    if (index < 0 || index >= count) {
        return 0;
    }

    Task* task = queue[index];
    for (int i = index + 1; i < count; ++i) {
        queue[i - 1] = queue[i];
    }
    --count;
    return task;
}

void remove_task(Task* tasks[], int& count, Task* task) {
    for (int i = 0; i < count; ++i) {
        if (tasks[i] == task) {
            for (int j = i + 1; j < count; ++j) {
                tasks[j - 1] = tasks[j];
            }
            --count;
            return;
        }
    }
}

} // namespace

RoundRobinScheduler::RoundRobinScheduler() : count(0) {}

const char* RoundRobinScheduler::name() const {
    return "Round Robin";
}

void RoundRobinScheduler::enqueue(Task* task, int) {
    push_back(ready, count, task);
}

Task* RoundRobinScheduler::pick_next(Core& core, int) {
    if (count == 0) {
        return 0;
    }
    core.slice_left = 10;
    return pop_front(ready, count);
}

void RoundRobinScheduler::account(Task*, double) {}

void RoundRobinScheduler::task_finished(Task*) {}

LinuxEEVDFScheduler::LinuxEEVDFScheduler()
    : count(0), active_count(0), virtual_time(0.0) {}

const char* LinuxEEVDFScheduler::name() const {
    return "Linux EEVDF";
}

void LinuxEEVDFScheduler::enqueue(Task* task, int) {
    bool already_active = false;
    for (int i = 0; i < active_count; ++i) {
        if (active[i] == task) {
            already_active = true;
            break;
        }
    }
    if (!already_active && active_count < MAX_TASKS) {
        if (task->vruntime < virtual_time) {
            task->vruntime = virtual_time;
        }
        active[active_count++] = task;
    }
    task->virtual_deadline =
        task->vruntime + 6.0 * 1024.0 / task_weight(*task);
    push_back(ready, count, task);
}

Task* LinuxEEVDFScheduler::pick_next(Core& core, int) {
    if (count == 0) {
        return 0;
    }

    int eligible_index = -1;
    int fallback_index = 0;
    for (int i = 0; i < count; ++i) {
        if (ready[i]->virtual_deadline < ready[fallback_index]->virtual_deadline) {
            fallback_index = i;
        }
        if (ready[i]->vruntime <= virtual_time + 0.000001 &&
            (eligible_index < 0 ||
             ready[i]->virtual_deadline < ready[eligible_index]->virtual_deadline)) {
            eligible_index = i;
        }
    }

    core.slice_left = 6;
    return remove_at(ready, count,
                     eligible_index >= 0 ? eligible_index : fallback_index);
}

void LinuxEEVDFScheduler::account(Task* task, double work_done) {
    double weight_sum = 0.0;
    for (int i = 0; i < active_count; ++i) {
        weight_sum += task_weight(*active[i]);
    }
    if (weight_sum <= 0.0) {
        return;
    }

    task->vruntime += work_done * 1024.0 / task_weight(*task);
    virtual_time += work_done * 1024.0 / weight_sum;
}

void LinuxEEVDFScheduler::task_finished(Task* task) {
    remove_task(active, active_count, task);
}

FreeBSDULEScheduler::FreeBSDULEScheduler()
    : interactive_count(0), batch_count(0) {}

const char* FreeBSDULEScheduler::name() const {
    return "FreeBSD ULE";
}

void FreeBSDULEScheduler::enqueue(Task* task, int) {
    if (task->foreground || task->interactivity >= 60) {
        push_back(interactive, interactive_count, task);
    } else {
        push_back(batch, batch_count, task);
    }
}

Task* FreeBSDULEScheduler::pick_next(Core& core, int) {
    if (interactive_count > 0) {
        core.slice_left = 4;
        return pop_front(interactive, interactive_count);
    }
    if (batch_count > 0) {
        core.slice_left = 10;
        return pop_front(batch, batch_count);
    }
    return 0;
}

void FreeBSDULEScheduler::account(Task*, double) {}

void FreeBSDULEScheduler::task_finished(Task*) {}

AndroidEASScheduler::AndroidEASScheduler()
    : performance_count(0), efficiency_count(0) {}

const char* AndroidEASScheduler::name() const {
    return "Android EAS + uclamp";
}

void AndroidEASScheduler::enqueue(Task* task, int) {
    if (task->foreground || task->priority <= 4) {
        push_back(performance, performance_count, task);
    } else {
        push_back(efficiency, efficiency_count, task);
    }
}

Task* AndroidEASScheduler::pick_next(Core& core, int) {
    if (core.type == 'P') {
        if (performance_count > 0) {
            core.slice_left = 5;
            return pop_front(performance, performance_count);
        }
    } else {
        if (efficiency_count > 0) {
            core.slice_left = 10;
            return pop_front(efficiency, efficiency_count);
        }
        if (performance_count > 0) {
            core.slice_left = 5;
            return pop_front(performance, performance_count);
        }
    }
    return 0;
}

void AndroidEASScheduler::account(Task*, double) {}

void AndroidEASScheduler::task_finished(Task*) {}
