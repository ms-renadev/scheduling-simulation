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

LinuxEEVDFScheduler::LinuxEEVDFScheduler() : count(0) {}

const char* LinuxEEVDFScheduler::name() const {
    return "Linux EEVDF";
}

void LinuxEEVDFScheduler::enqueue(Task* task, int) {
    push_back(ready, count, task);
}

Task* LinuxEEVDFScheduler::pick_next(Core& core, int) {
    if (count == 0) {
        return 0;
    }

    int best_index = 0;
    for (int i = 1; i < count; ++i) {
        if (ready[i]->virtual_deadline < ready[best_index]->virtual_deadline) {
            best_index = i;
        }
    }

    core.slice_left = 6;
    return remove_at(ready, count, best_index);
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
        if (efficiency_count > 0) {
            core.slice_left = 8;
            return pop_front(efficiency, efficiency_count);
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
