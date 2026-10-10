/*
 * sched_priority.c -- Priority scheduling, non-preemptive and preemptive.
 *
 * CONVENTION: a LOWER priority number means a HIGHER priority, which is the
 * convention used by the classic textbook examples (and by Linux nice
 * values).  Priority 0 is the most urgent, MAX_PRIORITY the least.
 *
 * Like SRTF, the preemptive variant only needs to re-decide at arrivals and
 * completions: priorities are static, so the process holding the CPU keeps
 * the best priority until somebody new turns up.
 */
#include "algorithms.h"
#include "sched_internal.h"

#include <stdlib.h>

static int pick_highest_priority(const Schedule *sch, const bool *finished,
                                 int now)
{
    int i, best = -1;
    for (i = 0; i < sch->n; i++) {
        if (finished[i]) continue;
        if (sch->procs[i].arrival > now) continue;
        if (best < 0) {
            best = i;
        } else if (sch->procs[i].priority < sch->procs[best].priority) {
            best = i;
        } else if (sch->procs[i].priority == sch->procs[best].priority) {
            if (sched_tie(sch, i, best) < 0) best = i;
        }
    }
    return best;
}

/* ------------------------------------------------------- non-preemptive */

bool sched_priority_np(Schedule *sch)
{
    bool *finished;
    int t, done = 0;
    char ready[READY_LIST_MAX];

    if (sch->n == 0) return true;

    finished = calloc((size_t)sch->n, sizeof *finished);
    if (finished == NULL) return false;

    t = sch->procs[0].arrival;
    {
        int i;
        for (i = 1; i < sch->n; i++)
            if (sch->procs[i].arrival < t) t = sch->procs[i].arrival;
    }

    schedule_tracef(sch, t,
                    "Priority (non-preemptive) start. Lower priority number "
                    "== higher priority. The choice is only made when the CPU "
                    "becomes free.");

    while (done < sch->n) {
        int pick = pick_highest_priority(sch, finished, t);

        if (pick < 0) {
            int next = sched_next_arrival(sch, finished, t);
            if (next < 0) break;
            sched_trace_idle(sch, t, next);
            if (!schedule_add_segment(sch, IDLE_INDEX, t, next)) {
                free(finished);
                return false;
            }
            t = next;
            continue;
        }

        sched_fmt_ready(sch, finished, NULL, t, pick, KEY_PRIORITY,
                        ready, sizeof ready);
        schedule_tracef(sch, t,
                        "t=%d: dispatch %s (priority %d, burst %d) -- best "
                        "priority among the arrived processes. Also waiting: "
                        "%s. It runs uninterrupted until t=%d.",
                        t, sch->procs[pick].id, sch->procs[pick].priority,
                        sch->procs[pick].burst, ready,
                        t + sch->procs[pick].burst);

        if (!schedule_add_segment(sch, pick, t, t + sch->procs[pick].burst)) {
            free(finished);
            return false;
        }
        t += sch->procs[pick].burst;
        finished[pick] = true;
        done++;

        schedule_tracef(sch, t, "t=%d: %s finishes (turnaround %d).",
                        t, sch->procs[pick].id,
                        t - sch->procs[pick].arrival);
    }

    free(finished);
    return true;
}

/* ----------------------------------------------------------- preemptive */

bool sched_priority_p(Schedule *sch)
{
    bool *finished;
    int  *remaining;
    int t, done = 0, prev = -1;
    char ready[READY_LIST_MAX];

    if (sch->n == 0) return true;

    finished = calloc((size_t)sch->n, sizeof *finished);
    remaining = malloc((size_t)sch->n * sizeof *remaining);
    if (finished == NULL || remaining == NULL) {
        free(finished);
        free(remaining);
        return false;
    }

    t = sch->procs[0].arrival;
    {
        int i;
        for (i = 0; i < sch->n; i++) {
            remaining[i] = sch->procs[i].burst;
            if (sch->procs[i].arrival < t) t = sch->procs[i].arrival;
        }
    }

    schedule_tracef(sch, t,
                    "Priority (preemptive) start. Lower priority number == "
                    "higher priority; an arrival with a strictly better "
                    "priority takes the CPU immediately.");

    while (done < sch->n) {
        int pick = pick_highest_priority(sch, finished, t);
        int next_arr, slice, run_until;

        if (pick < 0) {
            int next = sched_next_arrival(sch, finished, t);
            if (next < 0) break;
            sched_trace_idle(sch, t, next);
            if (!schedule_add_segment(sch, IDLE_INDEX, t, next)) goto oom;
            t = next;
            prev = -1;
            continue;
        }

        next_arr = sched_next_arrival(sch, finished, t);
        slice = remaining[pick];
        if (next_arr >= 0 && next_arr - t < slice) slice = next_arr - t;
        run_until = t + slice;

        if (pick != prev) {
            sched_fmt_ready(sch, finished, remaining, t, pick, KEY_PRIORITY,
                            ready, sizeof ready);
            if (prev >= 0) {
                schedule_tracef(sch, t,
                                "t=%d: PREEMPT %s (priority %d, remaining %d) "
                                "and dispatch %s (priority %d, remaining %d) "
                                "-- strictly better priority wins. Also "
                                "waiting: %s.",
                                t, sch->procs[prev].id,
                                sch->procs[prev].priority, remaining[prev],
                                sch->procs[pick].id,
                                sch->procs[pick].priority, remaining[pick],
                                ready);
            } else {
                schedule_tracef(sch, t,
                                "t=%d: dispatch %s (priority %d, remaining "
                                "%d) -- best priority among the arrived "
                                "processes. Also waiting: %s.",
                                t, sch->procs[pick].id,
                                sch->procs[pick].priority, remaining[pick],
                                ready);
            }
        }

        if (!schedule_add_segment(sch, pick, t, run_until)) goto oom;
        remaining[pick] -= slice;
        t = run_until;
        prev = pick;

        if (remaining[pick] == 0) {
            finished[pick] = true;
            done++;
            schedule_tracef(sch, t, "t=%d: %s finishes (turnaround %d).",
                            t, sch->procs[pick].id,
                            t - sch->procs[pick].arrival);
            prev = -1;
        }
    }

    free(finished);
    free(remaining);
    return true;

oom:
    free(finished);
    free(remaining);
    return false;
}
