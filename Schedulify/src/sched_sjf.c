/*
 * sched_sjf.c -- Shortest Job First (non-preemptive) and
 *                Shortest Remaining Time First (preemptive).
 *
 * The two belong together: SRTF is SJF re-evaluated every time the set of
 * ready processes changes.  Both pick the smallest key among the processes
 * that have already arrived, then fall back on the shared tie-break chain
 * (earlier arrival, then earlier input position).
 *
 * SRTF only has to re-decide at an arrival or at a completion.  While it
 * runs, the chosen process holds the minimum remaining time and its
 * remaining time only shrinks, so nothing can overtake it until a new
 * process shows up.  That lets the simulation advance in chunks instead of
 * single ticks without changing a single result.
 */
#include "algorithms.h"
#include "sched_internal.h"

#include <stdlib.h>

/* -------------------------------------------------- non-preemptive (SJF) */

/* Smallest burst among arrived, unfinished processes; -1 when none. */
static int pick_shortest_burst(const Schedule *sch, const bool *finished,
                               int now)
{
    int i, best = -1;
    for (i = 0; i < sch->n; i++) {
        if (finished[i]) continue;
        if (sch->procs[i].arrival > now) continue;
        if (best < 0) {
            best = i;
        } else if (sch->procs[i].burst < sch->procs[best].burst) {
            best = i;
        } else if (sch->procs[i].burst == sch->procs[best].burst) {
            if (sched_tie(sch, i, best) < 0) best = i;
        }
    }
    return best;
}

bool sched_sjf(Schedule *sch)
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
                    "SJF (non-preemptive) start. At every completion the "
                    "shortest burst among the arrived processes is chosen.");

    while (done < sch->n) {
        int pick = pick_shortest_burst(sch, finished, t);

        if (pick < 0) {
            int next = sched_next_arrival(sch, finished, t);
            if (next < 0) break;               /* cannot happen: done < n   */
            sched_trace_idle(sch, t, next);
            if (!schedule_add_segment(sch, IDLE_INDEX, t, next)) {
                free(finished);
                return false;
            }
            t = next;
            continue;
        }

        sched_fmt_ready(sch, finished, NULL, t, pick, KEY_BURST,
                        ready, sizeof ready);
        schedule_tracef(sch, t,
                        "t=%d: dispatch %s (burst %d) -- shortest burst of the "
                        "arrived processes. Also waiting: %s. It runs "
                        "uninterrupted until t=%d.",
                        t, sch->procs[pick].id, sch->procs[pick].burst,
                        ready, t + sch->procs[pick].burst);

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

/* ------------------------------------------------------ preemptive (SRTF) */

static int pick_shortest_remaining(const Schedule *sch, const bool *finished,
                                   const int *remaining, int now)
{
    int i, best = -1;
    for (i = 0; i < sch->n; i++) {
        if (finished[i]) continue;
        if (sch->procs[i].arrival > now) continue;
        if (best < 0) {
            best = i;
        } else if (remaining[i] < remaining[best]) {
            best = i;
        } else if (remaining[i] == remaining[best]) {
            if (sched_tie(sch, i, best) < 0) best = i;
        }
    }
    return best;
}

bool sched_srtf(Schedule *sch)
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
                    "SRTF (preemptive) start. The decision is revisited at "
                    "every arrival and every completion; a newcomer only "
                    "preempts when its remaining time is strictly smaller.");

    while (done < sch->n) {
        int pick = pick_shortest_remaining(sch, finished, remaining, t);
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

        /* Run until the next arrival could change the decision, or until the
         * process completes -- whichever comes first. */
        next_arr = sched_next_arrival(sch, finished, t);
        slice = remaining[pick];
        if (next_arr >= 0 && next_arr - t < slice) slice = next_arr - t;
        run_until = t + slice;

        if (pick != prev) {
            sched_fmt_ready(sch, finished, remaining, t, pick, KEY_REMAINING,
                            ready, sizeof ready);
            if (prev >= 0) {
                schedule_tracef(sch, t,
                                "t=%d: PREEMPT %s (remaining %d) and dispatch "
                                "%s (remaining %d) -- strictly smaller "
                                "remaining time wins. Also waiting: %s.",
                                t, sch->procs[prev].id, remaining[prev],
                                sch->procs[pick].id, remaining[pick], ready);
            } else {
                schedule_tracef(sch, t,
                                "t=%d: dispatch %s (remaining %d) -- smallest "
                                "remaining time of the arrived processes. "
                                "Also waiting: %s.",
                                t, sch->procs[pick].id, remaining[pick],
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
