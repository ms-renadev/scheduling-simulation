#!/usr/bin/env python3
"""
os_scheduler_sim.py
===================
Deterministic, discrete-time benchmark of four CPU schedulers on a heterogeneous
(big.LITTLE-style) 4-core mobile CPU, under the workload:

    "Streaming Spotify audio while chatting / loading media on Facebook
     Messenger, with background OS syncs."

Schedulers compared (all behind the same BaseScheduler interface):
    1. Round Robin (baseline)  - global FIFO queue, 8 ms quantum, core-agnostic
    2. Linux 6.6+ EEVDF        - weighted vruntime + virtual deadlines (heapq)
    3. FreeBSD ULE             - per-core interactive/batch queues, sleep/run
                                 interactivity score, balance on enqueue, idle steal
    4. Android EAS + uclamp    - split queues: UI/audio -> P cores, batch -> E cores

Only the Python standard library is used. Run:  python3 os_scheduler_sim.py

-------------------------------------------------------------------------------
MODELLING ASSUMPTIONS (state these on your slide - they make the test "fair")
-------------------------------------------------------------------------------
* 1 tick = 1 ms. Every scheduler sees the identical deep-copied task list.
* CPU burst lengths are WORK UNITS (= ms on a speed-1 E core). A P core
  (speed 2) retires 2 units per tick, an E core 1 unit per tick. A core that is
  busy for a tick is charged the full tick of power, even if the burst needed
  less than a tick's worth of work (discrete-time quantisation).
* Context switch: whenever a core starts a task different from the one it ran
  last, the core spends 1 tick doing zero work and burns 0.5 * power_cost.
* Idle cores draw IDLE_POWER_FRACTION * power_cost (default 0 = power-gated).
* "UI/Audio" tasks = priority <= 3 (Spotify decode + Messenger typing/scroll).
* Response time  = first tick a task ever executes - arrival.
* P95 latency    = 95th percentile of the *wake-up latency* (ready -> running)
  of every UI/audio CPU burst (including the first). This is what the ear/eye
  actually notices as stutter. (Nearest-rank percentile.)
* Jain's fairness is computed over the heavy batch tasks (priority >= 8) on
  x_i = ideal_time_i / turnaround_i, where ideal = work / P_speed + IO time
  (i.e. 1 / slowdown). 1.0 = every batch job is slowed down equally.
* CPU utilization = % of core-ticks spent doing useful work; context-switch
  overhead = % of core-ticks spent switching. Both are measured over
  (4 cores x makespan), so they stack to "busy" time.
"""

from __future__ import annotations

import copy
import heapq
import math
import random
import statistics
from abc import ABC, abstractmethod
from collections import deque
from dataclasses import dataclass
from typing import Deque, Dict, List, Optional, Tuple

# =============================================================================
# 0. Global configuration
# =============================================================================
SEED = 101
NUM_TASKS = 250
ARRIVAL_WINDOW_MS = 2000        # tasks arrive uniformly in [0, 2000] ms

CONTEXT_SWITCH_MS = 1           # cooldown ticks per switch
CS_POWER_FRACTION = 0.5         # switch burns 0.5 * power_cost per tick
IDLE_POWER_FRACTION = 0.0       # idle cores are power-gated

RR_QUANTUM_MS = 8

UI_PRIORITY_MAX = 3             # priority <= 3  -> UI / audio
BATCH_PRIORITY_MIN = 8          # priority >= 8  -> heavy background batch
MAX_TICKS = 500_000             # safety net against scheduler bugs

# (name, type, speed [work units/ms], power_cost [energy units/ms])
CORE_SPECS = [
    ("P0", "P", 2, 4.0),
    ("P1", "P", 2, 4.0),
    ("E0", "E", 1, 1.0),
    ("E1", "E", 1, 1.0),
]
P_SPEED = 2

# Reasons passed to scheduler.enqueue() so a policy knows *why* a task is ready.
NEW, WAKE, PREEMPT, SLICE = "new", "wake", "preempt", "slice"


# =============================================================================
# 1. Data model: tasks and cores
# =============================================================================
@dataclass
class Task:
    pid: int
    profile: str                          # spotify | messenger | media | sync
    priority: int                         # 1 (most urgent) .. 10 (background)
    arrival: int
    bursts: List[Tuple[str, int]]         # [("cpu", work), ("io", ms), ...]

    # ---- engine-owned bookkeeping ----
    cpu_idx: int = 0                      # index of the current CPU burst
    remaining: int = 0                    # work units left in current burst
    first_run: Optional[int] = None
    completion: Optional[int] = None
    ready_since: int = 0                  # tick the task became runnable
    blocked_at: int = 0                   # tick the task went to sleep (IO)
    awaiting_first_run: bool = True       # waiting for first tick of this burst

    # ---- scheduler scratch space (each policy uses what it needs) ----
    weight: float = 0.0                   # EEVDF
    vruntime: float = 0.0                 # EEVDF
    deadline: float = 0.0                 # EEVDF virtual deadline
    slice_left: int = 0                   # EEVDF remaining slice (ms)
    run_ticks: int = 0                    # ULE history: time spent running
    sleep_ticks: int = 0                  # ULE history: time spent sleeping
    sched_class: str = "batch"            # ULE: interactive | batch
    last_core: Optional[int] = None       # ULE: cache affinity
    seq: int = 0                          # Android: FIFO tie-breaker

    @property
    def is_ui_audio(self) -> bool:
        return self.priority <= UI_PRIORITY_MAX

    @property
    def is_batch(self) -> bool:
        return self.priority >= BATCH_PRIORITY_MIN

    def total_cpu_work(self) -> int:
        return sum(n for kind, n in self.bursts if kind == "cpu")

    def total_io(self) -> int:
        return sum(n for kind, n in self.bursts if kind == "io")


@dataclass
class Core:
    cid: int
    name: str
    kind: str                             # "P" or "E"
    speed: int
    power: float
    current: Optional[Task] = None
    slice_left: int = 0
    switch_left: int = 0                  # remaining context-switch cooldown
    last_pid: Optional[int] = None
    work_ticks: int = 0
    switch_ticks: int = 0
    idle_ticks: int = 0
    ctx_switches: int = 0
    energy: float = 0.0


# =============================================================================
# 2. Deterministic workload generator (seed = 101, 250 tasks)
# =============================================================================
PROFILE_MIX = [("spotify", 30), ("messenger", 35), ("media", 23), ("sync", 12)]  # %


def _allocate_counts(total: int, percents: List[int]) -> List[int]:
    """Largest-remainder rounding using integer maths (no float surprises)."""
    counts = [total * p // 100 for p in percents]
    remainders = [(total * p) % 100 for p in percents]
    order = sorted(range(len(percents)), key=lambda i: remainders[i], reverse=True)
    for i in order[: total - sum(counts)]:
        counts[i] += 1
    return counts


def _make_task_shape(profile: str, rng: random.Random) -> Tuple[int, List[Tuple[str, int]]]:
    """Return (priority, burst list) for one task of the given profile."""
    if profile == "spotify":
        # Strict-deadline tiny decode bursts: [2ms CPU, 20ms IO] x 6.
        bursts: List[Tuple[str, int]] = []
        for _ in range(6):
            bursts += [("cpu", 2), ("io", 20)]
        return 1, bursts[:-1]                       # no pointless trailing IO

    if profile == "messenger":
        # Typing / scrolling: 2-5ms CPU, then 12-30ms of "user tap" IO, 4-7 times.
        bursts = []
        for _ in range(rng.randint(4, 7)):
            bursts += [("cpu", rng.randint(2, 5)), ("io", rng.randint(12, 30))]
        return rng.randint(2, 3), bursts[:-1]

    if profile == "media":
        # Media preload / audio buffering: [10-22 CPU, 20-40 IO, 10-22 CPU].
        return rng.randint(4, 6), [("cpu", rng.randint(10, 22)),
                                   ("io", rng.randint(20, 40)),
                                   ("cpu", rng.randint(10, 22))]

    # "sync": background cloud sync / app update - heavy interference.
    return rng.randint(8, 10), [("cpu", rng.randint(70, 140)),
                                ("io", rng.randint(10, 20)),
                                ("cpu", rng.randint(70, 140))]


def generate_workload(seed: int = SEED, n: int = NUM_TASKS) -> List[Task]:
    rng = random.Random(seed)
    counts = _allocate_counts(n, [p for _, p in PROFILE_MIX])
    labels = [name for (name, _), c in zip(PROFILE_MIX, counts) for _ in range(c)]
    rng.shuffle(labels)                              # interleave profiles

    raw = []
    for label in labels:
        arrival = rng.randint(0, ARRIVAL_WINDOW_MS)
        priority, bursts = _make_task_shape(label, rng)
        raw.append((arrival, label, priority, bursts))
    raw.sort(key=lambda r: r[0])                     # stable: ties keep RNG order

    return [Task(pid=i, profile=lab, priority=pri, arrival=arr, bursts=b)
            for i, (arr, lab, pri, b) in enumerate(raw)]


# =============================================================================
# 3. Scheduler interface
# =============================================================================
class BaseScheduler(ABC):
    """
    The engine owns time, cores and context-switch accounting. A scheduler only
    answers four questions:
        enqueue()        - a task is runnable; where do I keep it?
        pick_next()      - this core is free; who runs?
        time_slice()     - how many ms may that task run before re-queueing?
        should_preempt() - a task is running; should it be kicked off now?
    """
    name = "Base"

    def __init__(self, cores: List[Core]):
        self.cores = cores

    @abstractmethod
    def enqueue(self, task: Task, now: int, reason: str, core: Optional[Core] = None) -> None: ...

    @abstractmethod
    def pick_next(self, core: Core, now: int) -> Optional[Task]: ...

    @abstractmethod
    def time_slice(self, task: Task, core: Core) -> int: ...

    def on_run(self, task: Task, core: Core, now: int) -> None:
        """Called once per tick a task actually executes (accounting hook)."""

    def should_preempt(self, core: Core, running: Task, now: int) -> bool:
        return False


# -----------------------------------------------------------------------------
# 3.1 Round Robin - the baseline
# -----------------------------------------------------------------------------
class RoundRobinScheduler(BaseScheduler):
    """
    One global FIFO queue. Every task gets a fixed 8 ms quantum, then goes to the
    back of the line. Priorities and core types are ignored, so a 2 ms audio
    burst may wait behind every heavy sync job in the queue ("convoy effect").
    """
    name = "Round Robin (Baseline)"

    def __init__(self, cores):
        super().__init__(cores)
        self.queue: Deque[Task] = deque()

    def enqueue(self, task, now, reason, core=None):
        self.queue.append(task)

    def pick_next(self, core, now):
        return self.queue.popleft() if self.queue else None

    def time_slice(self, task, core):
        return RR_QUANTUM_MS


# -----------------------------------------------------------------------------
# 3.2 Linux 6.6+ EEVDF (Earliest Eligible Virtual Deadline First)
# -----------------------------------------------------------------------------
NICE0_WEIGHT = 1024


def prio_to_weight(priority: int) -> float:
    """Each priority step ~ one nice level (x1.25 weight), as in Linux."""
    return NICE0_WEIGHT * (1.25 ** (5 - priority))   # prio1~2500 ... prio10~335


def eevdf_slice_ms(priority: int) -> int:
    """Latency-nice flavour: urgent tasks request a SHORT slice -> early deadline."""
    if priority <= 3:
        return 3
    return 6 if priority <= 6 else 10


class EEVDFScheduler(BaseScheduler):
    """
    Simplified EEVDF on one shared run-queue (a deliberate simplification of
    Linux's per-CPU queues so all 4 cores compete fairly).

      vruntime   : virtual CPU time, advances by delta * (1024 / weight), so heavy
                   (low-weight) background jobs "age" faster than audio jobs.
      deadline   : vruntime + slice / weight   (scaled by 1024)   <- earlier = more urgent
      eligible   : vruntime <= weighted-average vruntime of all runnable tasks,
                   i.e. the task has not already consumed more than its fair share.

    pick_next = among ELIGIBLE tasks choose the EARLIEST virtual deadline (min-heap).
    A woken task with an earlier deadline than the running one preempts it, so a
    2 ms audio burst jumps ahead of a 100 ms sync burst - yet the sync job is never
    starved, because its deadline keeps getting renewed and vruntime keeps it eligible.
    """
    name = "Linux (EEVDF)"

    def __init__(self, cores):
        super().__init__(cores)
        self.heap: List[Tuple[float, int, Task]] = []   # (virtual_deadline, pid, task)
        self.min_vruntime = 0.0

    # --- helpers ---------------------------------------------------------
    def _running(self) -> List[Task]:
        return [c.current for c in self.cores if c.current is not None]

    def _vslice(self, task: Task) -> float:
        return eevdf_slice_ms(task.priority) * NICE0_WEIGHT / task.weight

    def _avg_vruntime(self) -> float:
        pool = [e[2] for e in self.heap] + self._running()
        if not pool:
            return self.min_vruntime
        return sum(t.vruntime * t.weight for t in pool) / sum(t.weight for t in pool)

    def _update_min_vruntime(self) -> None:
        pool = [e[2].vruntime for e in self.heap] + [t.vruntime for t in self._running()]
        if pool:
            self.min_vruntime = max(self.min_vruntime, min(pool))   # monotonic

    def _pop_best(self, allow_ineligible: bool) -> Optional[Tuple[float, int, Task]]:
        """Pop the earliest-deadline ELIGIBLE entry from the min-heap."""
        avg_v = self._avg_vruntime()
        skipped, chosen = [], None
        while self.heap:
            entry = heapq.heappop(self.heap)
            if entry[2].vruntime <= avg_v + 1e-9:
                chosen = entry
                break
            skipped.append(entry)                 # over its fair share: not eligible
        if chosen is None and allow_ineligible and skipped:
            chosen = skipped.pop(0)               # stay work-conserving
        for e in skipped:
            heapq.heappush(self.heap, e)
        return chosen

    # --- scheduler interface ----------------------------------------------
    def enqueue(self, task, now, reason, core=None):
        if task.weight == 0.0:
            task.weight = prio_to_weight(task.priority)
        if reason in (NEW, WAKE):
            # Place the task near "now" in virtual time, but let a sleeper keep
            # at most one slice of credit (prevents banking unlimited lag).
            self._update_min_vruntime()
            task.vruntime = max(task.vruntime, self.min_vruntime - self._vslice(task))
            task.slice_left = eevdf_slice_ms(task.priority)
            task.deadline = task.vruntime + self._vslice(task)
        elif reason == SLICE:                     # slice used up -> request a new one
            task.slice_left = eevdf_slice_ms(task.priority)
            task.deadline = task.vruntime + self._vslice(task)
        # PREEMPT: keep the old deadline and the unused part of the slice
        heapq.heappush(self.heap, (task.deadline, task.pid, task))

    def pick_next(self, core, now):
        entry = self._pop_best(allow_ineligible=True)
        self._update_min_vruntime()
        return entry[2] if entry else None

    def time_slice(self, task, core):
        return task.slice_left

    def on_run(self, task, core, now):
        task.vruntime += NICE0_WEIGHT / task.weight    # weighted virtual time
        task.slice_left -= 1

    def should_preempt(self, core, running, now):
        entry = self._pop_best(allow_ineligible=False)
        if entry is None:
            return False
        heapq.heappush(self.heap, entry)               # peek only
        return entry[0] < running.deadline - 1e-9      # strictly earlier deadline


# -----------------------------------------------------------------------------
# 3.3 FreeBSD ULE
# -----------------------------------------------------------------------------
class ULEScheduler(BaseScheduler):
    """
    Per-core run-queues, each split into an INTERACTIVE and a BATCH queue.

    Interactivity score (0..100, lower = more interactive), from the sleep/run ratio:
        sleep > run :  score = 50 * run / sleep            (0..50)
        otherwise   :  score = 100 - 50 * sleep / run      (50..100)
        score += nice-like bias from static priority
    score < 30  -> interactive queue (always served first, may preempt batch)
    otherwise   -> batch queue (time slice shrinks as the queue grows).

    Balancing: (1) on enqueue pick the previous core if idle, else the least
    loaded core; (2) when a core runs dry it steals from the busiest core.
    ULE is NOT aware of P/E core speed - that is Android's advantage.
    """
    name = "FreeBSD (ULE)"
    INTERACT_THRESHOLD = 30
    INHERIT_RUN, INHERIT_SLEEP = 5, 20     # new threads inherit a "mostly sleeping" history
    HISTORY_MAX = 1000                     # decay (halve) history beyond this many ms
    INTERACTIVE_SLICE = 5
    BATCH_SLICE_MAX, BATCH_SLICE_MIN = 16, 4

    def __init__(self, cores):
        super().__init__(cores)
        self.interactive: Dict[int, Deque[Task]] = {c.cid: deque() for c in cores}
        self.batch: Dict[int, Deque[Task]] = {c.cid: deque() for c in cores}

    def _score(self, t: Task) -> float:
        run, sleep = max(t.run_ticks, 1), max(t.sleep_ticks, 1)
        base = 50.0 * run / sleep if sleep > run else 100.0 - 50.0 * sleep / run
        return base + (t.priority - 1) * 2          # static priority acts like "nice"

    def _load(self, core: Core) -> int:
        return (len(self.interactive[core.cid]) + len(self.batch[core.cid])
                + (1 if core.current is not None else 0))

    def _select_core(self, task: Task) -> Core:
        if task.last_core is not None and self._load(self.cores[task.last_core]) == 0:
            return self.cores[task.last_core]       # cache affinity if it is idle
        return min(self.cores, key=lambda c: (self._load(c), c.cid))   # least loaded

    def enqueue(self, task, now, reason, core=None):
        if reason == NEW:
            task.run_ticks, task.sleep_ticks = self.INHERIT_RUN, self.INHERIT_SLEEP
        elif reason == WAKE:
            task.sleep_ticks += now - task.blocked_at
        if task.run_ticks + task.sleep_ticks > self.HISTORY_MAX:   # forget old history
            task.run_ticks //= 2
            task.sleep_ticks //= 2

        task.sched_class = ("interactive" if self._score(task) < self.INTERACT_THRESHOLD
                            else "batch")
        target = core if core is not None else self._select_core(task)
        task.last_core = target.cid
        queue = (self.interactive if task.sched_class == "interactive" else self.batch)[target.cid]
        if reason == PREEMPT:
            queue.appendleft(task)                  # preempted tasks resume first
        else:
            queue.append(task)

    def _steal(self, thief: Core) -> Optional[Task]:
        donors = [c for c in self.cores if c.cid != thief.cid
                  and (self.interactive[c.cid] or self.batch[c.cid])]
        if not donors:
            return None
        donor = max(donors, key=lambda c: (len(self.interactive[c.cid]) + len(self.batch[c.cid]),
                                           -c.cid))
        queue = self.interactive[donor.cid] or self.batch[donor.cid]
        task = queue.popleft()
        task.last_core = thief.cid
        return task

    def pick_next(self, core, now):
        if self.interactive[core.cid]:
            return self.interactive[core.cid].popleft()   # interactive always first
        if self.batch[core.cid]:
            return self.batch[core.cid].popleft()
        return self._steal(core)                           # idle -> work stealing

    def time_slice(self, task, core):
        if task.sched_class == "interactive":
            return self.INTERACTIVE_SLICE
        queued = len(self.batch[core.cid])
        return max(self.BATCH_SLICE_MIN, self.BATCH_SLICE_MAX // (1 + queued))

    def on_run(self, task, core, now):
        task.run_ticks += 1

    def should_preempt(self, core, running, now):
        # An interactive task waiting on this core kicks out a running batch task.
        return running.sched_class == "batch" and bool(self.interactive[core.cid])


# -----------------------------------------------------------------------------
# 3.4 Android EAS + uclamp
# -----------------------------------------------------------------------------
class AndroidEASScheduler(BaseScheduler):
    """
    Energy-Aware Scheduling, modelled as two hardware-aware queues:

      * PERF queue (priority heap)  <- tasks with priority <= 4 ("uclamp.min" boosted:
        Spotify decode, Messenger UI, urgent media). Served only by P cores.
      * EFF  queue (FIFO)           <- everything else (background sync, bulk media).
        Pinned to E cores: ~half the energy per work unit (E: 1.0/1 = 1.0 energy per
        unit vs P: 4.0/2 = 2.0), at the cost of throughput.

    P cores preempt a less-urgent task the moment a more-urgent one is waiting.
    Real EAS falls back to load-balancing when the system is over-utilised; set
    ALLOW_SPILLOVER = True to let idle P cores help drain a long E backlog.
    """
    name = "Android (EAS + uclamp)"
    UCLAMP_PRIORITY_MAX = 4
    PERF_SLICE_MS = 6
    EFF_SLICE_MS = 12
    ALLOW_SPILLOVER = False
    SPILLOVER_BACKLOG = 6

    def __init__(self, cores):
        super().__init__(cores)
        self.perf: List[Tuple[int, int, Task]] = []   # heap of (priority, seq, task)
        self.eff: Deque[Task] = deque()
        self._seq = 0

    def enqueue(self, task, now, reason, core=None):
        if reason != PREEMPT:
            self._seq += 1
            task.seq = self._seq                       # keeps FIFO order among equals
        if task.priority <= self.UCLAMP_PRIORITY_MAX:
            heapq.heappush(self.perf, (task.priority, task.seq, task))
        elif reason == PREEMPT:
            self.eff.appendleft(task)
        else:
            self.eff.append(task)

    def pick_next(self, core, now):
        if core.kind == "P":
            if self.perf:
                return heapq.heappop(self.perf)[2]
            if self.ALLOW_SPILLOVER and len(self.eff) > self.SPILLOVER_BACKLOG:
                return self.eff.popleft()
            return None
        return self.eff.popleft() if self.eff else None   # E cores: batch only

    def time_slice(self, task, core):
        return self.PERF_SLICE_MS if core.kind == "P" else self.EFF_SLICE_MS

    def should_preempt(self, core, running, now):
        return core.kind == "P" and bool(self.perf) and self.perf[0][0] < running.priority


# =============================================================================
# 4. Simulation engine (identical for every scheduler)
# =============================================================================
def percentile(values: List[float], pct: float) -> float:
    """Nearest-rank percentile."""
    if not values:
        return 0.0
    ordered = sorted(values)
    return float(ordered[max(0, math.ceil(pct / 100.0 * len(ordered)) - 1)])


def jains_index(xs: List[float]) -> float:
    """Jain's fairness index: (sum x)^2 / (n * sum x^2), in (0, 1]."""
    sq = sum(x * x for x in xs)
    return (sum(xs) ** 2) / (len(xs) * sq) if sq > 0 else 1.0


def simulate(scheduler_cls, workload: List[Task]) -> dict:
    tasks = copy.deepcopy(workload)                       # same input for everyone
    cores = [Core(i, n, k, s, p) for i, (n, k, s, p) in enumerate(CORE_SPECS)]
    sched = scheduler_cls(cores)

    arrivals = sorted(tasks, key=lambda t: (t.arrival, t.pid))
    next_arrival = 0
    sleepers: List[Tuple[int, int, Task]] = []            # heap of (wake_tick, pid, task)
    finished = 0
    ui_latencies: List[int] = []
    now = 0

    while finished < len(tasks):
        if now > MAX_TICKS:
            raise RuntimeError(f"{scheduler_cls.name}: simulation did not terminate")

        # ---- 1. admit newly arrived tasks ---------------------------------
        while next_arrival < len(arrivals) and arrivals[next_arrival].arrival <= now:
            t = arrivals[next_arrival]
            next_arrival += 1
            t.remaining, t.ready_since, t.awaiting_first_run = t.bursts[0][1], now, True
            sched.enqueue(t, now, NEW)

        # ---- 2. IO completions: sleeping tasks become runnable ------------
        while sleepers and sleepers[0][0] <= now:
            _, _, t = heapq.heappop(sleepers)
            t.remaining, t.ready_since, t.awaiting_first_run = t.bursts[t.cpu_idx][1], now, True
            sched.enqueue(t, now, WAKE)

        # ---- 3. advance every core by one tick -----------------------------
        for core in cores:
            # (a) wake-up preemption (never in the middle of a context switch)
            if core.current is not None and core.switch_left == 0:
                if sched.should_preempt(core, core.current, now):
                    victim, core.current = core.current, None
                    sched.enqueue(victim, now, PREEMPT, core)

            # (b) idle core asks the scheduler for work
            if core.current is None:
                nxt = sched.pick_next(core, now)
                if nxt is not None:
                    core.current = nxt
                    core.slice_left = max(1, sched.time_slice(nxt, core))
                    if core.last_pid != nxt.pid:           # switching to a NEW task
                        core.switch_left = CONTEXT_SWITCH_MS
                        core.ctx_switches += 1
                    core.last_pid = nxt.pid

            # (c) nothing to run -> idle
            if core.current is None:
                core.idle_ticks += 1
                core.energy += IDLE_POWER_FRACTION * core.power
                continue

            # (d) context-switch cooldown: zero work, 0.5 * power
            if core.switch_left > 0:
                core.switch_left -= 1
                core.switch_ticks += 1
                core.energy += CS_POWER_FRACTION * core.power
                continue

            # (e) execute one tick of real work
            t = core.current
            if t.first_run is None:
                t.first_run = now
            if t.awaiting_first_run:                       # wake-up latency sample
                t.awaiting_first_run = False
                if t.is_ui_audio:
                    ui_latencies.append(now - t.ready_since)

            t.remaining -= min(core.speed, t.remaining)
            core.work_ticks += 1
            core.energy += core.power
            sched.on_run(t, core, now)

            if t.remaining == 0:                           # CPU burst finished
                core.current = None
                if t.cpu_idx == len(t.bursts) - 1:         # last burst -> task done
                    t.completion = now + 1
                    finished += 1
                else:                                      # go to sleep on IO
                    io_ms = t.bursts[t.cpu_idx + 1][1]
                    t.cpu_idx += 2
                    t.blocked_at = now + 1
                    heapq.heappush(sleepers, (now + 1 + io_ms, t.pid, t))
            else:
                core.slice_left -= 1
                if core.slice_left <= 0:                   # quantum expired
                    core.current = None
                    sched.enqueue(t, now + 1, SLICE, core)
        now += 1

    # ---- metrics ---------------------------------------------------------
    makespan = now
    core_ticks = len(cores) * makespan
    work = sum(c.work_ticks for c in cores)
    switching = sum(c.switch_ticks for c in cores)
    assert work + switching + sum(c.idle_ticks for c in cores) == core_ticks

    ui = [t for t in tasks if t.is_ui_audio]
    batch = [t for t in tasks if t.is_batch]
    fairness_x = [(t.total_cpu_work() / P_SPEED + t.total_io()) / (t.completion - t.arrival)
                  for t in batch]

    return {
        "name": scheduler_cls.name,
        "completion_ms": makespan,
        "avg_response_ms": statistics.mean(t.first_run - t.arrival for t in ui),
        "p95_latency_ms": percentile(ui_latencies, 95),
        "avg_turnaround_ms": statistics.mean(t.completion - t.arrival for t in tasks),
        "cpu_util_pct": 100.0 * work / core_ticks,
        "ctx_overhead_pct": 100.0 * switching / core_ticks,
        "jain_index": jains_index(fairness_x),
        "energy": sum(c.energy for c in cores),
        "ctx_switches": sum(c.ctx_switches for c in cores),
    }


# =============================================================================
# 5. Benchmark driver + ASCII table
# =============================================================================
SCHEDULERS = [RoundRobinScheduler, EEVDFScheduler, ULEScheduler, AndroidEASScheduler]


def run_benchmark() -> List[dict]:
    """Run all four schedulers on the same workload and return the `results` list."""
    workload = generate_workload()
    return [simulate(cls, workload) for cls in SCHEDULERS]


def print_workload_summary(workload: List[Task]) -> None:
    print(f"Workload: {len(workload)} tasks, seed {SEED}, arrivals in 0-{ARRIVAL_WINDOW_MS} ms")
    for name, _ in PROFILE_MIX:
        group = [t for t in workload if t.profile == name]
        print(f"  {name:<10} x{len(group):<3}  priority {min(t.priority for t in group)}-"
              f"{max(t.priority for t in group)}  total CPU work {sum(t.total_cpu_work() for t in group):>6} units")
    capacity = sum(s for _, _, s, _ in CORE_SPECS)
    total = sum(t.total_cpu_work() for t in workload)
    print(f"  Total work {total} units / CPU capacity {capacity} units/ms "
          f"= {total / capacity:.0f} ms minimum busy time\n")


def print_table(results: List[dict]) -> None:
    rows = [
        ("Total Completion Time (ms)",        "completion_ms",     "{:,.0f}"),
        ("Avg UI/Audio Response Time (ms)",   "avg_response_ms",   "{:,.2f}"),
        ("P95 UI/Audio Latency (ms)",         "p95_latency_ms",    "{:,.1f}"),
        ("Avg Turnaround Time (ms)",          "avg_turnaround_ms", "{:,.1f}"),
        ("CPU Utilization (%)",               "cpu_util_pct",      "{:.2f}"),
        ("Context Switch Overhead (%)",       "ctx_overhead_pct",  "{:.2f}"),
        ("Jain's Fairness Index (batch)",     "jain_index",        "{:.4f}"),
        ("Total Energy Consumed (Units)",     "energy",            "{:,.0f}"),
    ]
    label_w = max(len(r[0]) for r in rows) + 2
    col_w = max(len(r["name"]) for r in results) + 2
    rule = "+" + "-" * label_w + ("+" + "-" * col_w) * len(results) + "+"

    print(rule)
    print("|" + " Metric".ljust(label_w) + "".join("|" + r["name"].center(col_w) for r in results) + "|")
    print(rule.replace("-", "="))
    for label, key, fmt in rows:
        print("|" + (" " + label).ljust(label_w)
              + "".join("|" + fmt.format(r[key]).center(col_w) for r in results) + "|")
    print(rule)
    print("Lower is better for every row except CPU Utilization and Jain's Index.")

def plot_results(results: List[dict], filename: str = "scheduler_benchmark_charts.png") -> None:
    """Generate a 2x3 presentation dashboard of the benchmark metrics."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n[Note] Install matplotlib to generate charts: pip install matplotlib")
        return

    labels = ["Round Robin\n(Baseline)", "Linux\n(EEVDF)", "FreeBSD\n(ULE)", "Android\n(EAS + uclamp)"]
    colors = ["#2563EB", "#0D9488", "#D97706", "#7C3AED"]

    panels = [
        ("avg_response_ms",   "Avg UI/Audio Response Time",  "Milliseconds (lower is better)", "{:.2f}"),
        ("p95_latency_ms",    "P95 UI/Audio Wake-Up Latency","Milliseconds (lower is better)", "{:.1f}"),
        ("energy",            "Total Energy Consumed",       "Energy Units (lower is better)", "{:,.0f}"),
        ("completion_ms",     "Total Completion Time",       "Milliseconds (lower is better)", "{:,.0f}"),
        ("ctx_overhead_pct",  "Context Switch Overhead",     "% of Core-Ticks (lower is better)","{:.2f}%"),
        ("jain_index",        "Jain's Fairness Index (Batch)","Score 0–1 (higher is better)",   "{:.4f}"),
    ]

    fig, axes = plt.subplots(2, 3, figsize=(15, 8.5), dpi=200)
    fig.patch.set_facecolor("white")

    for ax, (key, title, ylabel, fmt) in zip(axes.flat, panels):
        vals = [r[key] for r in results]
        bars = ax.bar(labels, vals, color=colors, width=0.56, zorder=3)

        ax.set_title(title, fontsize=11.5, fontweight="bold", color="#0F172A", pad=10)
        ax.set_ylabel(ylabel, fontsize=9, color="#475569")
        ax.tick_params(axis="x", labelsize=8.5, colors="#1E293B")
        ax.tick_params(axis="y", labelsize=8.5, colors="#475569")
        ax.grid(axis="y", linestyle="--", alpha=0.45, color="#CBD5E1", zorder=0)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.spines["left"].set_color("#94A3B8")
        ax.spines["bottom"].set_color("#94A3B8")

        # Add headroom above tallest bar for value labels
        max_v = max(vals) if max(vals) > 0 else 1.0
        ax.set_ylim(0, max_v * 1.18)

        for bar, v in zip(bars, vals):
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + max_v * 0.03,
                fmt.format(v),
                ha="center",
                va="bottom",
                fontsize=8.5,
                fontweight="bold",
                color="#0F172A",
            )

    fig.suptitle(
        "Mobile OS CPU Scheduler Benchmark (250 Tasks • 4-Core big.LITTLE • Seed 101)",
        fontsize=14,
        fontweight="bold",
        color="#0F172A",
        y=0.98,
    )
    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(filename, dpi=200)
    print(f"\nSaved presentation charts to: {filename}")

if __name__ == "__main__":
    print_workload_summary(generate_workload())
    results = run_benchmark()
    print_table(results)
    plot_results(results)
