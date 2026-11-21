#ifndef THREAD_H
#define THREAD_H

#include <iostream>

enum class QueueClass {
    REALTIME = 0,
    TIMESHARE = 1,
    IDLE = 2
};

class Thread {
public:
    int tid;
    int arrival_time;
    double burst_time;
    double remaining_work;

    QueueClass q_class;
    int interactivity_score;
    double nice_weight;
    double vruntime;

    bool is_foreground;
    int start_time;
    int completion_time;

    Thread(int id,
           int arrival,
           double burst,
           QueueClass ule_class = QueueClass::TIMESHARE,
           int ule_interactivity = 50,
           double linux_weight = 1.0,
           bool android_fg = false);

    void executeTick(int current_time, double core_speed = 1.0);
    bool isCompleted() const;
    bool hasArrived(int current_time) const;
};

#endif // THREAD_H