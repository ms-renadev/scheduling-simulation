# Fair OS Scheduler Simulation

This project compares representative scheduling policies for Linux, FreeBSD,
Android, and a Round Robin baseline. Every policy receives the exact same
deterministic workload and the same heterogeneous four-core topology.

## Schedulers

- Round Robin baseline
- Linux-style EEVDF virtual-deadline approximation
- FreeBSD ULE interactive/batch queues
- Android EAS + uclamp foreground/performance and efficiency queues

## Metrics

The simulation records average and P95 response time, average turnaround,
CPU utilization, context-switch overhead, Jain fairness, and an energy
estimate.

The Android and FreeBSD implementations are educational approximations, not
kernel-accurate models. They use the public scheduling concepts described for
the platforms while keeping the workload and clock model controlled.

## Build and run

```sh
make
make run
```

Results are written to `scheduler_results.csv`.
