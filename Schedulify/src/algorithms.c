/*
 * algorithms.c -- Algorithm registry, shared helpers, and the run entry point.
 */
#include "algorithms.h"
#include "sched_internal.h"
#include "metrics.h"
#include "validate.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- registry */

static const AlgorithmInfo TABLE[ALGO_COUNT] = {
    {   ALGO_FCFS, "FCFS", "First Come First Served", "non-preemptive",
        false, false,
        "runs processes in arrival order, each to completion",
        "Pick the arrived process with the earliest arrival time; ties go to "
        "the process listed first in the input. Once dispatched a process "
        "keeps the CPU until its burst is finished."
    },
    {   ALGO_SJF, "SJF", "Shortest Job First", "non-preemptive",
        false, false,
        "runs the shortest available burst first, to completion",
        "Among the processes that have arrived, pick the smallest burst time; "
        "ties go to the earlier arrival, then to the earlier input position. "
        "A dispatched process is never interrupted, so a shorter job that "
        "arrives mid-burst has to wait."
    },
    {   ALGO_SRTF, "SRTF", "Shortest Remaining Time First", "preemptive",
        true, false,
        "preemptive SJF: the smallest remaining time always holds the CPU",
        "At every arrival the process with the smallest remaining time takes "
        "the CPU; ties go to the earlier arrival, then to the earlier input "
        "position. A newcomer preempts the running process only when its "
        "remaining time is strictly smaller, so equal values never cause a "
        "pointless switch."
    },
    {   ALGO_PRIORITY_NP, "PRIO-NP", "Priority Scheduling", "non-preemptive",
        false, false,
        "runs the highest-priority available process, to completion",
        "Among the processes that have arrived, pick the smallest priority "
        "number (lower number == higher priority); ties go to the earlier "
        "arrival, then to the earlier input position. A dispatched process "
        "keeps the CPU until its burst is finished."
    },
    {   ALGO_PRIORITY_P, "PRIO-P", "Priority Scheduling", "preemptive",
        true, false,
        "preemptive priority: a higher-priority arrival takes the CPU at once",
        "At every arrival the smallest priority number takes the CPU (lower "
        "number == higher priority); ties go to the earlier arrival, then to "
        "the earlier input position. A newcomer preempts the running process "
        "only when its priority is strictly higher."
    },
    {   ALGO_RR, "RR", "Round Robin", "preemptive",
        true, true,
        "gives each process one time quantum from a FIFO ready queue",
        "The ready queue is strictly FIFO. A dispatched process runs for "
        "min(quantum, remaining). On quantum expiry, processes that arrived "
        "during the slice -- including exactly at its final instant -- are "
        "queued first, then the preempted process is appended behind them. "
        "Processes arriving at the same instant enter in input order."
    }
};

const AlgorithmInfo *algo_info(AlgorithmId id)
{
    if (id < 0 || id >= ALGO_COUNT) return &TABLE[0];
    return &TABLE[id];
}

const char *algo_name(AlgorithmId id)
{
    return algo_info(id)->name;
}

const char *algo_abbrev(AlgorithmId id)
{
    return algo_info(id)->abbrev;
}

bool algo_needs_quantum(AlgorithmId id)
{
    return algo_info(id)->needs_quantum;
}

/* ---------------------------------------------------------------- helpers */

int sched_tie(const Schedule *sch, int a, int b)
{
    if (sch->procs[a].arrival != sch->procs[b].arrival)
        return (sch->procs[a].arrival < sch->procs[b].arrival) ? -1 : 1;
    return (a < b) ? -1 : 1;
}

void sched_arrival_order(const Schedule *sch, int *order)
{
    int i, j;
    for (i = 0; i < sch->n; i++) order[i] = i;
    /* Insertion sort keyed on arrival; stable, so equal arrivals keep the
     * input order and rule 3 of the tie-break chain is satisfied. */
    for (i = 1; i < sch->n; i++) {
        int key = order[i];
        for (j = i - 1;
             j >= 0 && sch->procs[order[j]].arrival > sch->procs[key].arrival;
             j--) {
            order[j + 1] = order[j];
        }
        order[j + 1] = key;
    }
}

int sched_next_arrival(const Schedule *sch, const bool *finished, int now)
{
    int i, best = -1;
    for (i = 0; i < sch->n; i++) {
        if (finished != NULL && finished[i]) continue;
        if (sch->procs[i].arrival <= now) continue;
        if (best < 0 || sch->procs[i].arrival < best)
            best = sch->procs[i].arrival;
    }
    return best;
}

void sched_fmt_ready(const Schedule *sch, const bool *finished,
                     const int *remaining, int now, int exclude,
                     ReadyKey key, char *buf, size_t sz)
{
    int i;
    size_t used = 0;
    int shown = 0;

    if (sz == 0) return;
    buf[0] = '\0';

    for (i = 0; i < sch->n; i++) {
        char item[48];
        int len;

        if (i == exclude) continue;
        if (finished != NULL && finished[i]) continue;
        if (sch->procs[i].arrival > now) continue;
        if (remaining != NULL && remaining[i] <= 0) continue;

        switch (key) {
        case KEY_REMAINING:
            len = snprintf(item, sizeof item, "%s(rem %d)", sch->procs[i].id,
                           (remaining != NULL) ? remaining[i]
                                               : sch->procs[i].burst);
            break;
        case KEY_BURST:
            len = snprintf(item, sizeof item, "%s(burst %d)",
                           sch->procs[i].id, sch->procs[i].burst);
            break;
        case KEY_PRIORITY:
            len = snprintf(item, sizeof item, "%s(prio %d)",
                           sch->procs[i].id, sch->procs[i].priority);
            break;
        case KEY_ARRIVAL:
        default:
            len = snprintf(item, sizeof item, "%s(arr %d)",
                           sch->procs[i].id, sch->procs[i].arrival);
            break;
        }
        if (len < 0) continue;

        if (used + (size_t)len + 3 >= sz) {
            snprintf(buf + used, sz - used, "%s...", (shown > 0) ? ", " : "");
            return;
        }
        if (shown > 0) {
            buf[used++] = ',';
            buf[used++] = ' ';
            buf[used] = '\0';
        }
        memcpy(buf + used, item, (size_t)len + 1);
        used += (size_t)len;
        shown++;
    }
    if (shown == 0) snprintf(buf, sz, "empty");
}

void sched_trace_idle(Schedule *sch, int from, int to)
{
    schedule_tracef(sch, from,
                    "CPU IDLE from t=%d to t=%d: every process that has "
                    "arrived is already finished, so the CPU waits %d time "
                    "unit(s) for the next arrival.",
                    from, to, to - from);
}

/* ------------------------------------------------------------- run facade */

bool algo_run(Schedule *sch, const Dataset *ds, AlgorithmId algo, int quantum,
              bool enable_trace, char *err, size_t errsz)
{
    bool ok;
    char verr[ERR_MSG_MAX];

    if (!validate_dataset(ds, verr, sizeof verr)) {
        snprintf(err, errsz, "%s", verr);
        memset(sch, 0, sizeof *sch);
        return false;
    }
    if (algo_needs_quantum(algo)) {
        if (!validate_quantum(quantum, verr, sizeof verr)) {
            snprintf(err, errsz, "%s", verr);
            memset(sch, 0, sizeof *sch);
            return false;
        }
    } else {
        quantum = 0;
    }

    if (!schedule_init(sch, ds, algo, quantum, enable_trace)) {
        snprintf(err, errsz, "out of memory while preparing the simulation");
        return false;
    }

    switch (algo) {
    case ALGO_FCFS:        ok = sched_fcfs(sch);        break;
    case ALGO_SJF:         ok = sched_sjf(sch);         break;
    case ALGO_SRTF:        ok = sched_srtf(sch);        break;
    case ALGO_PRIORITY_NP: ok = sched_priority_np(sch); break;
    case ALGO_PRIORITY_P:  ok = sched_priority_p(sch);  break;
    case ALGO_RR:          ok = sched_round_robin(sch); break;
    default:
        snprintf(err, errsz, "unknown algorithm id %d", (int)algo);
        return false;
    }

    if (!ok) {
        snprintf(err, errsz, "out of memory while building the timeline");
        return false;
    }
    if (!metrics_finalize(sch)) {
        snprintf(err, errsz, "internal error: %s", metrics_last_error());
        return false;
    }
    return true;
}

/* Case-insensitive name -> id.  The command line and the web API both go
 * through this, so `--algo prio-p` and {"algorithm":"prio-p"} agree. */
bool algo_from_name(const char *name, AlgorithmId *out)
{
    static const struct { const char *name; AlgorithmId id; } MAP[] = {
        { "fcfs",       ALGO_FCFS },
        { "fifo",       ALGO_FCFS },
        { "sjf",        ALGO_SJF },
        { "sjn",        ALGO_SJF },
        { "srtf",       ALGO_SRTF },
        { "srt",        ALGO_SRTF },
        { "prio-np",    ALGO_PRIORITY_NP },
        { "priority",   ALGO_PRIORITY_NP },
        { "prio",       ALGO_PRIORITY_NP },
        { "prio-p",     ALGO_PRIORITY_P },
        { "priority-p", ALGO_PRIORITY_P },
        { "rr",         ALGO_RR },
        { "roundrobin", ALGO_RR }
    };
    char lower[32];
    size_t i;

    if (name == NULL || name[0] == '\0') return false;
    if (strlen(name) >= sizeof lower) return false;
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[i] = '\0';

    for (i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        if (strcmp(lower, MAP[i].name) == 0) { *out = MAP[i].id; return true; }
    }
    return false;
}
