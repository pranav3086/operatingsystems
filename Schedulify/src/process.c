/*
 * process.c -- Dataset container and Schedule/Trace lifecycle.
 */
#include "process.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ utils */

static void copy_bounded(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0;
    if (dst_sz == 0) return;
    if (src != NULL) {
        for (; src[i] != '\0' && i + 1 < dst_sz; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int ci_equal(const char *a, const char *b)
{
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i]))
            return 0;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* ---------------------------------------------------------------- Dataset */

void dataset_init(Dataset *ds, const char *name)
{
    ds->items = NULL;
    ds->count = 0;
    ds->capacity = 0;
    copy_bounded(ds->name, sizeof ds->name, (name != NULL) ? name : "untitled");
}

void dataset_free(Dataset *ds)
{
    if (ds == NULL) return;
    free(ds->items);
    ds->items = NULL;
    ds->count = 0;
    ds->capacity = 0;
}

void dataset_clear(Dataset *ds)
{
    ds->count = 0;
}

void dataset_set_name(Dataset *ds, const char *name)
{
    copy_bounded(ds->name, sizeof ds->name, name);
}

static bool dataset_reserve(Dataset *ds, int needed)
{
    int new_cap;
    Process *grown;

    if (needed <= ds->capacity) return true;
    if (needed > MAX_PROCESSES) return false;

    new_cap = (ds->capacity > 0) ? ds->capacity * 2 : 8;
    if (new_cap < needed) new_cap = needed;
    if (new_cap > MAX_PROCESSES) new_cap = MAX_PROCESSES;

    grown = realloc(ds->items, (size_t)new_cap * sizeof *grown);
    if (grown == NULL) return false;

    ds->items = grown;
    ds->capacity = new_cap;
    return true;
}

bool dataset_add(Dataset *ds, const Process *p)
{
    if (ds->count >= MAX_PROCESSES) return false;
    if (!dataset_reserve(ds, ds->count + 1)) return false;
    ds->items[ds->count] = *p;
    ds->items[ds->count].id[PROC_ID_MAX - 1] = '\0';
    ds->count++;
    return true;
}

bool dataset_remove(Dataset *ds, int index)
{
    int i;
    if (index < 0 || index >= ds->count) return false;
    for (i = index; i < ds->count - 1; i++) ds->items[i] = ds->items[i + 1];
    ds->count--;
    return true;
}

int dataset_find_id(const Dataset *ds, const char *id)
{
    int i;
    if (id == NULL) return -1;
    for (i = 0; i < ds->count; i++) {
        if (ci_equal(ds->items[i].id, id)) return i;
    }
    return -1;
}

bool dataset_clone(Dataset *dst, const Dataset *src)
{
    int i;
    dataset_init(dst, src->name);
    if (src->count == 0) return true;
    if (!dataset_reserve(dst, src->count)) return false;
    for (i = 0; i < src->count; i++) dst->items[i] = src->items[i];
    dst->count = src->count;
    return true;
}

/* Insertion sort: stable, and n is small enough that O(n^2) is irrelevant.
 * Stability is what makes "equal arrival -> original input order" hold. */
void dataset_sort_by_arrival(Dataset *ds)
{
    int i, j;
    for (i = 1; i < ds->count; i++) {
        Process key = ds->items[i];
        for (j = i - 1; j >= 0 && ds->items[j].arrival > key.arrival; j--) {
            ds->items[j + 1] = ds->items[j];
        }
        ds->items[j + 1] = key;
    }
}

int dataset_min_arrival(const Dataset *ds)
{
    int i, best;
    if (ds->count == 0) return 0;
    best = ds->items[0].arrival;
    for (i = 1; i < ds->count; i++) {
        if (ds->items[i].arrival < best) best = ds->items[i].arrival;
    }
    return best;
}

int dataset_total_burst(const Dataset *ds)
{
    int i, sum = 0;
    for (i = 0; i < ds->count; i++) sum += ds->items[i].burst;
    return sum;
}

/* ------------------------------------------------------------------ Trace */

static void trace_init(Trace *tr, bool enabled)
{
    tr->items = NULL;
    tr->count = 0;
    tr->capacity = 0;
    tr->enabled = enabled;
}

static void trace_free(Trace *tr)
{
    free(tr->items);
    tr->items = NULL;
    tr->count = 0;
    tr->capacity = 0;
}

static bool trace_reserve(Trace *tr, int needed)
{
    int new_cap;
    TraceEvent *grown;

    if (needed <= tr->capacity) return true;
    new_cap = (tr->capacity > 0) ? tr->capacity * 2 : 32;
    if (new_cap < needed) new_cap = needed;

    grown = realloc(tr->items, (size_t)new_cap * sizeof *grown);
    if (grown == NULL) return false;

    tr->items = grown;
    tr->capacity = new_cap;
    return true;
}

/* --------------------------------------------------------------- Schedule */

bool schedule_init(Schedule *sch, const Dataset *ds, AlgorithmId algo,
                   int quantum, bool enable_trace)
{
    int i;

    memset(sch, 0, sizeof *sch);
    trace_init(&sch->trace, enable_trace);

    sch->algo = algo;
    sch->quantum = quantum;
    sch->n = ds->count;
    copy_bounded(sch->dataset_name, sizeof sch->dataset_name, ds->name);

    if (ds->count > 0) {
        sch->procs = malloc((size_t)ds->count * sizeof *sch->procs);
        sch->stats = malloc((size_t)ds->count * sizeof *sch->stats);
        if (sch->procs == NULL || sch->stats == NULL) {
            schedule_free(sch);
            memset(sch, 0, sizeof *sch);
            trace_init(&sch->trace, enable_trace);
            return false;
        }
        for (i = 0; i < ds->count; i++) {
            sch->procs[i] = ds->items[i];
            sch->stats[i].completion = 0;
            sch->stats[i].first_start = -1;
            sch->stats[i].turnaround = 0;
            sch->stats[i].waiting = 0;
            sch->stats[i].response = 0;
            sch->stats[i].executed = 0;
        }
    }
    return true;
}

void schedule_free(Schedule *sch)
{
    if (sch == NULL) return;
    free(sch->procs);
    free(sch->stats);
    free(sch->segments);
    sch->procs = NULL;
    sch->stats = NULL;
    sch->segments = NULL;
    sch->n = 0;
    sch->seg_count = 0;
    sch->seg_capacity = 0;
    trace_free(&sch->trace);
}

static bool schedule_reserve_segments(Schedule *sch, int needed)
{
    int new_cap;
    Segment *grown;

    if (needed <= sch->seg_capacity) return true;
    new_cap = (sch->seg_capacity > 0) ? sch->seg_capacity * 2 : 16;
    if (new_cap < needed) new_cap = needed;

    grown = realloc(sch->segments, (size_t)new_cap * sizeof *grown);
    if (grown == NULL) return false;

    sch->segments = grown;
    sch->seg_capacity = new_cap;
    return true;
}

bool schedule_add_segment(Schedule *sch, int proc, int start, int end)
{
    if (end <= start) return true;  /* zero-length slices carry no meaning */

    if (sch->seg_count > 0) {
        Segment *last = &sch->segments[sch->seg_count - 1];
        if (last->proc == proc && last->end == start) {
            last->end = end;        /* merge: one logical run of the CPU   */
            return true;
        }
    }
    if (!schedule_reserve_segments(sch, sch->seg_count + 1)) return false;
    sch->segments[sch->seg_count].proc = proc;
    sch->segments[sch->seg_count].start = start;
    sch->segments[sch->seg_count].end = end;
    sch->seg_count++;
    return true;
}

void schedule_tracef(Schedule *sch, int time, const char *fmt, ...)
{
    va_list ap;
    TraceEvent *ev;

    if (!sch->trace.enabled) return;
    if (!trace_reserve(&sch->trace, sch->trace.count + 1)) return;

    ev = &sch->trace.items[sch->trace.count];
    ev->time = time;
    va_start(ap, fmt);
    vsnprintf(ev->text, sizeof ev->text, fmt, ap);
    va_end(ap);
    sch->trace.count++;
}
