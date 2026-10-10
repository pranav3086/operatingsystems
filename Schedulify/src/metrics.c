/*
 * metrics.c -- Timeline -> metrics.
 */
#include "metrics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_err[200] = "";

const char *metrics_last_error(void)
{
    return g_err;
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

bool metrics_finalize(Schedule *sch)
{
    int i;
    int window_start, window_end;
    long sum_completion = 0, sum_turnaround = 0, sum_waiting = 0;
    long sum_response = 0;
    int prev_running = IDLE_INDEX;

    g_err[0] = '\0';
    sch->finalized = false;
    sch->busy_time = 0;
    sch->idle_time = 0;
    sch->context_switches = 0;
    sch->preemptions = 0;
    sch->avg_completion = 0.0;
    sch->avg_turnaround = 0.0;
    sch->avg_waiting = 0.0;
    sch->avg_response = 0.0;
    sch->cpu_utilization = 0.0;
    sch->throughput = 0.0;
    sch->window_start = 0;
    sch->window_end = 0;
    sch->total_time = 0;

    if (sch->n == 0) {
        sch->finalized = true;
        return true;
    }

    for (i = 0; i < sch->n; i++) {
        sch->stats[i].completion = -1;
        sch->stats[i].first_start = -1;
        sch->stats[i].executed = 0;
        sch->stats[i].turnaround = 0;
        sch->stats[i].waiting = 0;
        sch->stats[i].response = 0;
    }

    window_start = sch->procs[0].arrival;
    for (i = 1; i < sch->n; i++) {
        if (sch->procs[i].arrival < window_start)
            window_start = sch->procs[i].arrival;
    }

    if (sch->seg_count == 0) {
        fail("the algorithm produced an empty timeline for %d process(es)",
             sch->n);
        return false;
    }
    if (sch->segments[0].start != window_start) {
        fail("the timeline starts at t=%d but the earliest arrival is t=%d",
             sch->segments[0].start, window_start);
        return false;
    }

    /* ---- single pass over the timeline ---- */
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        int dur = s->end - s->start;

        if (dur <= 0) {
            fail("segment %d has non-positive length [%d,%d)", i,
                 s->start, s->end);
            return false;
        }
        if (i > 0 && s->start != sch->segments[i - 1].end) {
            fail("timeline gap or overlap between segment %d (ends %d) and "
                 "segment %d (starts %d)", i - 1, sch->segments[i - 1].end,
                 i, s->start);
            return false;
        }
        if (s->proc == IDLE_INDEX) {
            sch->idle_time += dur;
            continue;
        }
        if (s->proc < 0 || s->proc >= sch->n) {
            fail("segment %d refers to unknown process index %d", i, s->proc);
            return false;
        }

        {
            ProcStats *st = &sch->stats[s->proc];
            const Process *p = &sch->procs[s->proc];

            if (s->start < p->arrival) {
                fail("process %s ran at t=%d but only arrives at t=%d",
                     p->id, s->start, p->arrival);
                return false;
            }
            if (st->first_start < 0) st->first_start = s->start;

            st->executed += dur;
            if (st->executed > p->burst) {
                fail("process %s was given %d time units but its burst is %d",
                     p->id, st->executed, p->burst);
                return false;
            }
            st->completion = s->end;
            sch->busy_time += dur;

            /* A hand-over to a different process is one context switch.
             * Segments are pre-merged, so consecutive entries never carry
             * the same process index unless an idle gap separated them. */
            if (prev_running != IDLE_INDEX && prev_running != s->proc)
                sch->context_switches++;

            /* The previous process left with work outstanding -> preemption.
             * This is evaluated when we see the *next* process take over,
             * so a quantum expiry with nobody else ready is not counted. */
            if (prev_running != IDLE_INDEX && prev_running != s->proc) {
                const ProcStats *pst = &sch->stats[prev_running];
                if (pst->executed < sch->procs[prev_running].burst)
                    sch->preemptions++;
            }
            prev_running = s->proc;
        }
    }

    window_end = sch->segments[sch->seg_count - 1].end;

    /* A trailing idle segment would mean the simulation ran past the last
     * completion; trim it out of the window so utilization stays honest. */
    if (sch->segments[sch->seg_count - 1].proc == IDLE_INDEX) {
        fail("the timeline ends with an idle segment [%d,%d)",
             sch->segments[sch->seg_count - 1].start, window_end);
        return false;
    }

    for (i = 0; i < sch->n; i++) {
        ProcStats *st = &sch->stats[i];
        const Process *p = &sch->procs[i];

        if (st->completion < 0) {
            fail("process %s never ran", p->id);
            return false;
        }
        if (st->executed != p->burst) {
            fail("process %s received %d of its %d time units",
                 p->id, st->executed, p->burst);
            return false;
        }
        st->turnaround = st->completion - p->arrival;
        st->waiting = st->turnaround - p->burst;
        st->response = st->first_start - p->arrival;

        sum_completion += st->completion;
        sum_turnaround += st->turnaround;
        sum_waiting += st->waiting;
        sum_response += st->response;
    }

    sch->window_start = window_start;
    sch->window_end = window_end;
    sch->total_time = window_end - window_start;

    sch->avg_completion = (double)sum_completion / sch->n;
    sch->avg_turnaround = (double)sum_turnaround / sch->n;
    sch->avg_waiting    = (double)sum_waiting / sch->n;
    sch->avg_response   = (double)sum_response / sch->n;

    if (sch->total_time > 0) {
        sch->cpu_utilization = 100.0 * (double)sch->busy_time
                             / (double)sch->total_time;
        sch->throughput = (double)sch->n / (double)sch->total_time;
    } else {
        sch->cpu_utilization = 0.0;
        sch->throughput = 0.0;
    }

    /* Consistency check: idle + busy must fill the window exactly. */
    if (sch->busy_time + sch->idle_time != sch->total_time) {
        fail("busy (%d) + idle (%d) != window length (%d)",
             sch->busy_time, sch->idle_time, sch->total_time);
        return false;
    }

    sch->finalized = true;
    return true;
}

int metrics_best_process_by_turnaround(const Schedule *sch)
{
    int i, best;
    if (sch->n <= 0) return -1;
    best = 0;
    for (i = 1; i < sch->n; i++) {
        if (sch->stats[i].turnaround < sch->stats[best].turnaround) best = i;
    }
    return best;
}
