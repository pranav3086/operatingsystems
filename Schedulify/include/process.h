/*
 * process.h -- Core data structures for Schedulify.
 *
 * Everything the simulator manipulates is defined here:
 *
 *   Process    a single job description supplied by the user
 *   Dataset    a growable collection of processes (the "workload")
 *   Segment    one contiguous slice of CPU time on the timeline
 *   ProcStats  per-process results derived from the timeline
 *   Trace      ordered list of human-readable scheduler decisions
 *   Schedule   the complete result of running one algorithm on one dataset
 *
 * Design note: the scheduling algorithms only ever produce a *timeline*
 * (an ordered list of Segments).  Every metric -- completion time,
 * turnaround, waiting, response, idle time, context switches and
 * preemptions -- is afterwards derived from that timeline by metrics.c.
 * Keeping one single source of truth means the six algorithms cannot
 * disagree with each other about how a number is defined.
 */
#ifndef SCHEDULIFY_PROCESS_H
#define SCHEDULIFY_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

#define SCHEDULIFY_VERSION "1.0.0"

/* ---- Limits (all input is validated against these) ---------------------- */
#define PROC_ID_MAX       16     /* including the NUL terminator            */
#define DATASET_NAME_MAX  64
#define MAX_PROCESSES     512
#define MIN_ARRIVAL       0
#define MAX_ARRIVAL       100000
#define MIN_BURST         1      /* a zero-length burst is not a job        */
#define MAX_BURST         10000
#define MIN_PRIORITY      0
#define MAX_PRIORITY      99
#define MIN_QUANTUM       1
#define MAX_QUANTUM       1000
#define DEFAULT_QUANTUM   2

/* Sentinel used in Segment.proc to mean "the CPU was idle". */
#define IDLE_INDEX        (-1)

/* ---- Process ------------------------------------------------------------ */
typedef struct {
    char id[PROC_ID_MAX];  /* user-visible label, e.g. "P1"                 */
    int  arrival;          /* time the process enters the ready queue       */
    int  burst;            /* total CPU time required                       */
    int  priority;         /* LOWER number == HIGHER priority (documented)  */
} Process;

/* ---- Dataset: a dynamically allocated array of processes ---------------- */
typedef struct {
    Process *items;
    int      count;
    int      capacity;
    char     name[DATASET_NAME_MAX];
} Dataset;

void dataset_init(Dataset *ds, const char *name);
void dataset_free(Dataset *ds);
void dataset_clear(Dataset *ds);
void dataset_set_name(Dataset *ds, const char *name);

/* Appends a copy of p.  Returns false if the dataset is full or out of
 * memory.  Does NOT check for duplicate ids -- use dataset_find_id first. */
bool dataset_add(Dataset *ds, const Process *p);

/* Removes the process at index.  Returns false if index is out of range. */
bool dataset_remove(Dataset *ds, int index);

/* Returns the index of the process whose id matches (case-insensitive),
 * or -1 when absent. */
int dataset_find_id(const Dataset *ds, const char *id);

/* Deep copy.  dst is initialised by this call; returns false on OOM. */
bool dataset_clone(Dataset *dst, const Dataset *src);

/* Stable sort by arrival time, then by original input order. */
void dataset_sort_by_arrival(Dataset *ds);

/* Smallest arrival time in the dataset (0 when the dataset is empty). */
int dataset_min_arrival(const Dataset *ds);

/* Sum of all burst times. */
int dataset_total_burst(const Dataset *ds);

/* ---- Algorithms --------------------------------------------------------- */
typedef enum {
    ALGO_FCFS = 0,
    ALGO_SJF,              /* Shortest Job First, non-preemptive            */
    ALGO_SRTF,             /* Shortest Remaining Time First, preemptive     */
    ALGO_PRIORITY_NP,      /* Priority, non-preemptive                      */
    ALGO_PRIORITY_P,       /* Priority, preemptive                          */
    ALGO_RR,               /* Round Robin                                   */
    ALGO_COUNT
} AlgorithmId;

/* ---- Timeline ----------------------------------------------------------- */
typedef struct {
    int proc;   /* index into Schedule.procs, or IDLE_INDEX for an idle gap  */
    int start;  /* inclusive                                                 */
    int end;    /* exclusive; end > start always holds                       */
} Segment;

/* ---- Per-process results ------------------------------------------------ */
typedef struct {
    int completion;   /* absolute time the process finished                  */
    int first_start;  /* absolute time it first got the CPU (-1 if never)    */
    int turnaround;   /* completion - arrival                                */
    int waiting;      /* turnaround - burst                                  */
    int response;     /* first_start - arrival                               */
    int executed;     /* CPU time actually granted (== burst when finished)  */
} ProcStats;

/* ---- Trace (educational step-by-step mode) ------------------------------ */
#define TRACE_TEXT_MAX 220

typedef struct {
    int  time;                    /* simulation clock when it happened      */
    char text[TRACE_TEXT_MAX];    /* already-formatted explanation          */
} TraceEvent;

typedef struct {
    TraceEvent *items;
    int         count;
    int         capacity;
    bool        enabled;          /* when false, sched_tracef() is a no-op  */
} Trace;

/* ---- Schedule: the full result of one run ------------------------------- */
typedef struct {
    AlgorithmId algo;
    int         quantum;          /* meaningful for ALGO_RR only            */
    char        dataset_name[DATASET_NAME_MAX];

    Process   *procs;             /* owned copy of the input, count == n    */
    ProcStats *stats;             /* n entries, filled by metrics_finalize  */
    int        n;

    Segment   *segments;          /* the timeline, in increasing time order */
    int        seg_count;
    int        seg_capacity;

    Trace      trace;

    /* ---- aggregate metrics (metrics_finalize) ---- */
    int    window_start;          /* earliest arrival time                  */
    int    window_end;            /* last completion time                   */
    int    total_time;            /* window_end - window_start              */
    int    busy_time;             /* CPU time spent executing processes     */
    int    idle_time;             /* total_time - busy_time                 */
    int    context_switches;
    int    preemptions;
    double avg_completion;
    double avg_turnaround;
    double avg_waiting;
    double avg_response;
    double cpu_utilization;       /* percent, 0..100                        */
    double throughput;            /* processes completed per time unit      */
    bool   finalized;
} Schedule;

/* Allocates procs/stats/segments for the given dataset and copies the
 * processes in.  Returns false on OOM (sch is left zeroed and safe to
 * pass to schedule_free).  enable_trace turns on decision logging. */
bool schedule_init(Schedule *sch, const Dataset *ds, AlgorithmId algo,
                   int quantum, bool enable_trace);

void schedule_free(Schedule *sch);

/* Appends [start,end) for proc to the timeline.  Adjacent segments that
 * belong to the same process (or are both idle) are merged, which is what
 * makes the context-switch and preemption counts correct for Round Robin.
 * Returns false on OOM. */
bool schedule_add_segment(Schedule *sch, int proc, int start, int end);

/* Appends a formatted explanation to the trace (no-op when disabled). */
void schedule_tracef(Schedule *sch, int time, const char *fmt, ...);

#endif /* SCHEDULIFY_PROCESS_H */
