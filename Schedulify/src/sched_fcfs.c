/*
 * sched_fcfs.c -- First Come First Served.
 *
 * The simplest non-preemptive policy: sort by arrival and run each process to
 * completion.  Idle gaps appear whenever the next arrival is in the future.
 */
#include "algorithms.h"
#include "sched_internal.h"

#include <stdlib.h>

bool sched_fcfs(Schedule *sch)
{
    int *order;
    int t, k;

    if (sch->n == 0) return true;

    order = malloc((size_t)sch->n * sizeof *order);
    if (order == NULL) return false;

    sched_arrival_order(sch, order);

    t = sch->procs[order[0]].arrival;
    schedule_tracef(sch, t,
                    "FCFS start. The ready queue is filled in arrival order; "
                    "each process keeps the CPU for its whole burst.");

    for (k = 0; k < sch->n; k++) {
        int i = order[k];
        const Process *p = &sch->procs[i];

        if (t < p->arrival) {
            sched_trace_idle(sch, t, p->arrival);
            if (!schedule_add_segment(sch, IDLE_INDEX, t, p->arrival)) {
                free(order);
                return false;
            }
            t = p->arrival;
        }

        schedule_tracef(sch, t,
                        "t=%d: dispatch %s (arrived t=%d, burst %d) because it "
                        "is the earliest unserved arrival. It will hold the "
                        "CPU until t=%d.",
                        t, p->id, p->arrival, p->burst, t + p->burst);

        if (!schedule_add_segment(sch, i, t, t + p->burst)) {
            free(order);
            return false;
        }
        t += p->burst;

        schedule_tracef(sch, t, "t=%d: %s finishes (turnaround %d).",
                        t, p->id, t - p->arrival);
    }

    free(order);
    return true;
}
