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
- Linux-style EEVDF virtual-deadline approximation
- FreeBSD ULE interactive/batch queues
- Android EAS + uclamp foreground/performance and efficiency queues

## Metrics

The simulation records average and P95 response time, average turnaround,
CPU utilization, context-switch event count, Jain fairness, and an energy
estimate.

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
