/*
 * datasets.c -- Predefined examples and the deterministic random generator.
 */
#include "datasets.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name;
    const char *description;
    int         quantum;
    int         count;
    Process     items[10];
} Example;

/* Priority is given for every process even where the algorithm ignores it,
 * so that "compare all" has something meaningful to work with. */
static const Example EXAMPLES[] = {
    {
        "Convoy effect",
        "One long job arrives first and three short ones queue behind it. "
        "FCFS makes everybody wait; SJF and SRTF do not.",
        4, 4,
        { { "P1", 0, 24, 3 }, { "P2", 0, 3, 1 },
          { "P3", 0, 3, 2 }, { "P4", 0, 4, 4 } }
    },
    {
        "Textbook SJF / SRTF",
        "The standard four-process example used to contrast non-preemptive "
        "SJF with preemptive SRTF. Hand-worked answers are in the README.",
        2, 4,
        { { "P1", 0, 7, 2 }, { "P2", 2, 4, 1 },
          { "P3", 4, 1, 3 }, { "P4", 5, 4, 2 } }
    },
    {
        "Textbook priority",
        "Five processes that all arrive at t=0 with distinct priorities: the "
        "schedule is simply priority order. Lower number == higher priority.",
        2, 5,
        { { "P1", 0, 10, 3 }, { "P2", 0, 1, 1 }, { "P3", 0, 2, 4 },
          { "P4", 0, 1, 5 }, { "P5", 0, 5, 2 } }
    },
    {
        "Round Robin showcase",
        "A long job and two short ones, all ready at t=0. Try quantum 1, 4 "
        "and 24 to watch response time trade off against context switches.",
        4, 3,
        { { "P1", 0, 24, 2 }, { "P2", 0, 3, 1 }, { "P3", 0, 3, 3 } }
    },
    {
        "Idle CPU gaps",
        "Arrivals are far apart, so the CPU has nothing to run for long "
        "stretches. Watch utilization drop below 100%.",
        3, 4,
        { { "P1", 0, 3, 2 }, { "P2", 7, 2, 1 },
          { "P3", 9, 4, 3 }, { "P4", 20, 2, 2 } }
    },
    {
        "Simultaneous arrivals and ties",
        "Every process arrives at t=0, several share a burst time and "
        "several share a priority. This is the dataset that exercises the "
        "tie-break chain: equal keys fall back to input order.",
        2, 6,
        { { "P1", 0, 4, 2 }, { "P2", 0, 4, 2 }, { "P3", 0, 2, 1 },
          { "P4", 0, 2, 1 }, { "P5", 0, 6, 3 }, { "P6", 0, 6, 3 } }
    },
    {
        "Preemption storm",
        "Each arrival has a shorter burst than the one before it, so SRTF "
        "preempts on almost every arrival while FCFS never does.",
        2, 5,
        { { "P1", 0, 9, 5 }, { "P2", 1, 7, 4 }, { "P3", 2, 5, 3 },
          { "P4", 3, 3, 2 }, { "P5", 4, 1, 1 } }
    },
    {
        "Single process",
        "The degenerate case: one process, no competition. Every algorithm "
        "must agree, and nothing may be reported as a context switch.",
        2, 1,
        { { "P1", 0, 5, 0 } }
    },
    {
        "Mixed workload (8 processes)",
        "A broader workload with staggered arrivals, a wide spread of bursts "
        "and repeated priorities -- a realistic stress test for the "
        "comparison table.",
        3, 8,
        { { "P1", 0, 6, 2 }, { "P2", 1, 8, 1 }, { "P3", 2, 7, 3 },
          { "P4", 3, 3, 1 }, { "P5", 6, 4, 4 }, { "P6", 10, 5, 2 },
          { "P7", 12, 2, 0 }, { "P8", 15, 9, 3 } }
    },
    {
        "Late arrival after an idle CPU",
        "Three quick jobs finish, the CPU sits idle for a while, then one "
        "last job turns up. Checks that idle time never counts as waiting "
        "time for anyone.",
        2, 4,
        { { "P1", 0, 2, 1 }, { "P2", 1, 2, 2 },
          { "P3", 2, 2, 3 }, { "P4", 15, 3, 1 } }
    }
};

static const int EXAMPLE_COUNT = (int)(sizeof EXAMPLES / sizeof EXAMPLES[0]);

int examples_count(void)
{
    return EXAMPLE_COUNT;
}

const char *example_name(int index)
{
    if (index < 0 || index >= EXAMPLE_COUNT) return "?";
    return EXAMPLES[index].name;
}

const char *example_description(int index)
{
    if (index < 0 || index >= EXAMPLE_COUNT) return "";
    return EXAMPLES[index].description;
}

int example_quantum(int index)
{
    if (index < 0 || index >= EXAMPLE_COUNT) return DEFAULT_QUANTUM;
    return EXAMPLES[index].quantum;
}

bool example_load(int index, Dataset *ds)
{
    int i;

    if (index < 0 || index >= EXAMPLE_COUNT) return false;

    dataset_init(ds, EXAMPLES[index].name);
    for (i = 0; i < EXAMPLES[index].count; i++) {
        if (!dataset_add(ds, &EXAMPLES[index].items[i])) {
            dataset_free(ds);
            dataset_init(ds, EXAMPLES[index].name);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- random ---- */

/* xorshift64*: tiny, no library dependency, identical output everywhere. */
static unsigned long long xs_next(unsigned long long *s)
{
    unsigned long long x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static int xs_range(unsigned long long *s, int lo, int hi)
{
    unsigned long long span;
    if (hi <= lo) return lo;
    span = (unsigned long long)(hi - lo) + 1ULL;
    return lo + (int)((xs_next(s) >> 11) % span);
}

unsigned long dataset_random_seed(void)
{
    return (unsigned long)time(NULL);
}

bool dataset_random(Dataset *ds, int n, unsigned long seed, int max_arrival,
                    int max_burst, int max_priority)
{
    unsigned long long state;
    char name[DATASET_NAME_MAX];
    int i;

    if (n < 1) n = 1;
    if (n > MAX_PROCESSES) n = MAX_PROCESSES;
    if (max_arrival < MIN_ARRIVAL) max_arrival = MIN_ARRIVAL;
    if (max_arrival > MAX_ARRIVAL) max_arrival = MAX_ARRIVAL;
    if (max_burst < MIN_BURST) max_burst = MIN_BURST;
    if (max_burst > MAX_BURST) max_burst = MAX_BURST;
    if (max_priority < MIN_PRIORITY) max_priority = MIN_PRIORITY;
    if (max_priority > MAX_PRIORITY) max_priority = MAX_PRIORITY;

    /* Seed 0 would make xorshift degenerate, so fold it away. */
    state = (unsigned long long)seed * 6364136223846793005ULL
          + 1442695040888963407ULL;
    if (state == 0) state = 0x9E3779B97F4A7C15ULL;

    snprintf(name, sizeof name, "random-%lu (n=%d)", seed, n);
    dataset_init(ds, name);

    for (i = 0; i < n; i++) {
        Process p;
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "P%d", i + 1);
        p.arrival = xs_range(&state, MIN_ARRIVAL, max_arrival);
        p.burst = xs_range(&state, MIN_BURST, max_burst);
        p.priority = xs_range(&state, MIN_PRIORITY, max_priority);
        if (!dataset_add(ds, &p)) {
            dataset_free(ds);
            dataset_init(ds, name);
            return false;
        }
    }

    /* Presenting them in arrival order makes the generated table easier to
     * read; the tie-break chain still falls back on this listed order. */
    dataset_sort_by_arrival(ds);

    /* At least one process must start the clock at the earliest arrival; the
     * sort guarantees items[0] holds it, so nothing else is needed. */
    return true;
}
