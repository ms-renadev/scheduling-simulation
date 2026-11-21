#include "Thread.h"
#include <iostream>

Thread::Thread(int id,
               int arrival,
               double burst,
               QueueClass ule_class,
               int ule_interactivity,
               double linux_weight,
               bool android_fg)
    : tid(id),
      arrival_time(arrival),
      burst_time(burst),
      remaining_work(burst),
      q_class(ule_class),
      interactivity_score(ule_interactivity),
      nice_weight(linux_weight),
      vruntime(0.0),
      is_foreground(android_fg),
      start_time(-1),
      completion_time(-1) {}

void Thread::executeTick(int current_time, double core_speed) {
    if (start_time == -1) {
        start_time = current_time;
    }

    double work_done = (remaining_work < core_speed) ? remaining_work : core_speed;
    remaining_work -= work_done;

    vruntime += work_done / nice_weight;

    if (remaining_work <= 1e-9) {
        remaining_work = 0.0;
        completion_time = current_time + 1;
    }
}

bool Thread::isCompleted() const {
    return remaining_work <= 1e-9;
}

bool Thread::hasArrived(int current_time) const {
    return arrival_time <= current_time;
}