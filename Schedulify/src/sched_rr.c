/*
 * sched_rr.c -- Round Robin.
 *
 * A strictly FIFO ready queue plus a time quantum.  The only place where
 * implementations usually disagree is the instant a quantum expires at
 * exactly the same time as an arrival.  Schedulify resolves it the way the
 * standard textbook traces do:
 *
 *     at t = end of quantum
 *       1. every process whose arrival time is <= t is appended to the queue,
 *          in arrival order (input order for simultaneous arrivals),
 *       2. then the process whose quantum just expired is appended behind
 *          them, if it still has work left.
 *
 * So a newcomer that shows up exactly at the boundary gets the CPU before the
 * process that has just been running.  If nobody else is waiting, the running
 * process is simply re-dispatched; schedule_add_segment() merges the two
 * slices into one, which keeps that free of a bogus context switch.
 */
#include "algorithms.h"
#include "sched_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring buffer sized n+1: a process is in the queue at most once at a time. */
typedef struct {
    int *slot;
    int  cap;
    int  head;
    int  tail;
    int  count;
} Fifo;

static bool fifo_init(Fifo *q, int n)
{
    q->cap = n + 1;
    q->slot = malloc((size_t)q->cap * sizeof *q->slot);
    q->head = q->tail = q->count = 0;
    return q->slot != NULL;
}

static void fifo_free(Fifo *q)
{
    free(q->slot);
    q->slot = NULL;
}

static void fifo_push(Fifo *q, int v)
{
    if (q->count >= q->cap) return;      /* unreachable: cap == n + 1      */
    q->slot[q->tail] = v;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
}

static int fifo_pop(Fifo *q)
{
    int v;
    if (q->count == 0) return -1;
    v = q->slot[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return v;
}

static void fifo_describe(const Fifo *q, const Schedule *sch,
                          const int *remaining, char *buf, size_t sz)
{
    int k;
    size_t used = 0;

    if (sz == 0) return;
    if (q->count == 0) {
        snprintf(buf, sz, "empty");
        return;
    }
    buf[0] = '\0';
    for (k = 0; k < q->count; k++) {
        int i = q->slot[(q->head + k) % q->cap];
        char item[48];
        int len = snprintf(item, sizeof item, "%s%s(rem %d)",
                           (k > 0) ? " <- " : "", sch->procs[i].id,
                           remaining[i]);
        if (len < 0) continue;
        if (used + (size_t)len + 4 >= sz) {
            snprintf(buf + used, sz - used, " <- ...");
            return;
        }
        memcpy(buf + used, item, (size_t)len + 1);
        used += (size_t)len;
    }
}

bool sched_round_robin(Schedule *sch)
{
    Fifo q;
    int *order;
    int *remaining;
    int cursor = 0;            /* next process in arrival order to admit   */
    int t, done = 0;
    char qdesc[READY_LIST_MAX];

    if (sch->n == 0) return true;

    q.slot = NULL;
    order = malloc((size_t)sch->n * sizeof *order);
    remaining = malloc((size_t)sch->n * sizeof *remaining);
    if (order == NULL || remaining == NULL || !fifo_init(&q, sch->n)) {
        free(order);
        free(remaining);
        fifo_free(&q);
        return false;
    }

    sched_arrival_order(sch, order);
    {
        int i;
        for (i = 0; i < sch->n; i++) remaining[i] = sch->procs[i].burst;
    }

    t = sch->procs[order[0]].arrival;

    schedule_tracef(sch, t,
                    "Round Robin start, time quantum = %d. The ready queue is "
                    "FIFO; a process that uses up its quantum goes to the "
                    "back, behind anything that arrived during the slice.",
                    sch->quantum);

    /* Admit everything that has arrived by time t. */
    while (cursor < sch->n && sch->procs[order[cursor]].arrival <= t) {
        fifo_push(&q, order[cursor]);
        schedule_tracef(sch, t, "t=%d: %s arrives and joins the ready queue.",
                        t, sch->procs[order[cursor]].id);
        cursor++;
    }

    while (done < sch->n) {
        int pick, slice, run_until;

        if (q.count == 0) {
            int next;
            if (cursor >= sch->n) break;      /* cannot happen: done < n   */
            next = sch->procs[order[cursor]].arrival;
            sched_trace_idle(sch, t, next);
            if (!schedule_add_segment(sch, IDLE_INDEX, t, next)) goto oom;
            t = next;
            while (cursor < sch->n && sch->procs[order[cursor]].arrival <= t) {
                fifo_push(&q, order[cursor]);
                schedule_tracef(sch, t,
                                "t=%d: %s arrives and joins the ready queue.",
                                t, sch->procs[order[cursor]].id);
                cursor++;
            }
            continue;
        }

        pick = fifo_pop(&q);
        slice = (remaining[pick] < sch->quantum) ? remaining[pick]
                                                 : sch->quantum;
        run_until = t + slice;

        fifo_describe(&q, sch, remaining, qdesc, sizeof qdesc);
        schedule_tracef(sch, t,
                        "t=%d: dispatch %s from the front of the queue "
                        "(remaining %d, quantum %d) -> runs for %d unit(s) "
                        "until t=%d. Queue behind it: %s.",
                        t, sch->procs[pick].id, remaining[pick], sch->quantum,
                        slice, run_until, qdesc);

        if (!schedule_add_segment(sch, pick, t, run_until)) goto oom;
        remaining[pick] -= slice;
        t = run_until;

        /* Step 1: admit arrivals up to and including the boundary. */
        while (cursor < sch->n && sch->procs[order[cursor]].arrival <= t) {
            fifo_push(&q, order[cursor]);
            schedule_tracef(sch, t,
                            "t=%d: %s arrives and joins the ready queue "
                            "(ahead of any process whose quantum just "
                            "expired).",
                            t, sch->procs[order[cursor]].id);
            cursor++;
        }

        if (remaining[pick] == 0) {
            done++;
            schedule_tracef(sch, t, "t=%d: %s finishes (turnaround %d).",
                            t, sch->procs[pick].id,
                            t - sch->procs[pick].arrival);
        } else {
            /* Step 2: the preempted process re-joins at the back. */
            fifo_push(&q, pick);
            fifo_describe(&q, sch, remaining, qdesc, sizeof qdesc);
            if (q.count == 1) {
                schedule_tracef(sch, t,
                                "t=%d: %s used its whole quantum (remaining "
                                "%d) but no other process is ready, so it "
                                "keeps the CPU -- no context switch.",
                                t, sch->procs[pick].id, remaining[pick]);
            } else {
                schedule_tracef(sch, t,
                                "t=%d: quantum expired, %s goes to the back of "
                                "the queue with %d unit(s) left. Queue: %s.",
                                t, sch->procs[pick].id, remaining[pick],
                                qdesc);
            }
        }
    }

    free(order);
    free(remaining);
    fifo_free(&q);
    return true;

oom:
    free(order);
    free(remaining);
    fifo_free(&q);
    return false;
}
