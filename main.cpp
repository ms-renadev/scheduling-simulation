// --- TODO 1: 2. LINUX EEVDF (Array Min-Search imbes na priority_queue) ---
class LinuxEEVDFScheduler : public BaseScheduler {
    Task* ready[MAX_QUEUE];
    int count = 0;

public:
    std::string getName() override { return "2. Linux (EEVDF)"; }

    void enqueue(Task* t, int time) override {
        if (count >= MAX_QUEUE) return;
        double weight = 1024.0 / std::pow(1.25, t->priority - 1);
        t->virtual_deadline = t->vruntime + (6.0 * (1024.0 / weight));
        ready[count++] = t;
    }

    Task* pickNext(Core& core, int time) override {
        if (count == 0) return nullptr;
        int best_idx = 0;
        for (int i = 1; i < count; ++i) {
            if (ready[i]->virtual_deadline < ready[best_idx]->virtual_deadline) {
                best_idx = i;
            }
        }
        Task* best = ready[best_idx];
        ready[best_idx] = ready[--count];
        core.slice_left = 6;
        return best;
    }
};

// --- TODO 2: 3. FREEBSD ULE (Gamit ang ArrayQueue) ---
class FreeBSDULEScheduler : public BaseScheduler {
    ArrayQueue interactive_q;
    ArrayQueue batch_q;

public:
    std::string getName() override { return "3. FreeBSD (ULE)"; }

    void enqueue(Task* t, int time) override {
        if (t->total_sleep >= t->total_cpu || t->priority <= 3) {
            interactive_q.push(t);
        } else {
            batch_q.push(t);
        }
    }

    Task* pickNext(Core& core, int time) override {
        if (!interactive_q.empty()) {
            core.slice_left = 4;
            return interactive_q.pop();
        }
        if (!batch_q.empty()) {
            core.slice_left = 10;
            return batch_q.pop();
        }
        return nullptr;
    }
};

// --- TODO 3: 4. ANDROID EAS + UCLAMP (Gamit ang ArrayQueue) ---
class AndroidEASScheduler : public BaseScheduler {
    ArrayQueue perf_q;
    ArrayQueue eff_q;

public:
    std::string getName() override { return "4. Android (EAS + uclamp)"; }

    void enqueue(Task* t, int time) override {
        if (t->priority <= 4) {
            perf_q.push(t);
        } else {
            eff_q.push(t);
        }
    }

    Task* pickNext(Core& core, int time) override {
        if (core.type == "P") {
            if (!perf_q.empty()) {
                core.slice_left = 5;
                return perf_q.pop();
            } else if (eff_q.size() > 6) {
                core.slice_left = 8;
                return eff_q.pop();
            }
        } else if (core.type == "E") {
            if (!eff_q.empty()) {
                core.slice_left = 10;
                return eff_q.pop();
            } else if (!perf_q.empty()) {
                core.slice_left = 5;
                return perf_q.pop();
            }
        }
        return nullptr;
    }
};