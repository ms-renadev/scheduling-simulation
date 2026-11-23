# Fair OS Scheduler Simulation

This project compares representative scheduling policies for Linux, FreeBSD,
Android, and a Round Robin baseline. Every policy receives the exact same
deterministic workload and the same heterogeneous four-core topology.

The C++ code uses only the standard `iostream` library. Tasks, cores, queues,
and results use fixed-size arrays; it does not depend on `vector`, `deque`,
`string`, or other standard library headers. The workload uses a deterministic
pseudo-random generator implemented in the project.

## Schedulers

- Round Robin baseline
- Linux-style EEVDF eligible virtual-deadline approximation
- FreeBSD ULE interactive/batch queues
- Android EAS + uclamp foreground/performance and efficiency queues

## Metrics

The simulation reports makespan, average and P95 response time, average
turnaround, CPU busy-time utilization, dispatch/context-switch events, a Jain
weighted-service fairness estimate, estimated energy, and the number of
completed tasks. `Jain*` compares each completed task's actual work with its
estimated priority-weighted fair-service opportunity. The ideal opportunity
is calculated from active-task weights and available core capacity per tick;
it is a teaching approximation, not a kernel-level fairness guarantee.
Utilization divides busy core-ticks by `makespan × core count`. The switch
count is task dispatches after each core's first dispatch; switch time is not
charged.

Tasks require burst work units. P cores perform 1.8 work units per tick and
consume 2.8 model energy units per busy tick; E cores perform 0.7 work units
per tick and consume 0.65 model energy units per busy tick. These illustrative
rates make placement affect throughput, utilization, and energy; they are not
hardware measurements.

The EEVDF approximation tracks weighted virtual runtime, advances a shared
virtual time, picks the earliest-deadline eligible task, and falls back to the
earliest deadline when no queued task is eligible. It is still a small
teaching model, not the Linux kernel implementation. Android's simplified
placement sends performance-class tasks to P cores and efficiency-class tasks
to E cores; E cores may also run performance tasks if needed.

The Android and FreeBSD implementations are educational approximations, not
kernel-accurate models. They use the public scheduling concepts described for
the platforms while keeping the workload and clock model controlled.

## Build and run

```sh
make
make run
```

Results are printed as a readable table in the terminal. The Makefile saves a
separate CSV copy to `scheduler_results.csv`.

The simulation supports up to 40 tasks and 4 cores, as set in
`include/scheduler_sim.h`.
