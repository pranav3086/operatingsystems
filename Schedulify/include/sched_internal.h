/*
 * sched_internal.h -- Helpers shared by the scheduler implementations.
 *
 * Not part of the public interface; only src/sched_*.c and src/algorithms.c
 * include this.  Keeping the tie-break chain and the ready-queue formatter
 * in one place is what guarantees the six algorithms stay consistent.
 */
#ifndef SCHEDULIFY_SCHED_INTERNAL_H
#define SCHEDULIFY_SCHED_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "process.h"

#define READY_LIST_MAX 160

typedef enum {
    KEY_ARRIVAL = 0,
    KEY_BURST,
    KEY_REMAINING,
    KEY_PRIORITY
} ReadyKey;

/* The shared tie-break chain for equal primary keys:
 * earlier arrival wins, then earlier input position.
 * Returns a negative value when a should go first. */
int sched_tie(const Schedule *sch, int a, int b);

/* Fills order[0..n-1] with process indices sorted by arrival time, ties
 * broken by input position (a stable sort). */
void sched_arrival_order(const Schedule *sch, int *order);

/* Earliest arrival time strictly greater than now among processes that are
 * not finished, or -1 when there is none. */
int sched_next_arrival(const Schedule *sch, const bool *finished, int now);

/* Writes something like "P2(rem 3), P4(rem 4)" describing the processes that
 * have arrived by `now`, are unfinished, and are not `exclude`.
 * remaining may be NULL for the keys that do not need it. */
void sched_fmt_ready(const Schedule *sch, const bool *finished,
                     const int *remaining, int now, int exclude,
                     ReadyKey key, char *buf, size_t sz);

/* Convenience: the trace line emitted when the CPU has nothing to run. */
void sched_trace_idle(Schedule *sch, int from, int to);

#endif /* SCHEDULIFY_SCHED_INTERNAL_H */
