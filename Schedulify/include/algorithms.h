/*
 * algorithms.h -- The six schedulers and the registry that describes them.
 *
 * Every scheduler has the same shape: it receives a Schedule that already
 * holds a private copy of the workload, and its only job is to write the
 * execution timeline with schedule_add_segment().  It never computes a
 * metric -- metrics.c does that from the timeline.
 *
 * DETERMINISM
 * -----------
 * Two runs on the same dataset always produce byte-identical output.  Every
 * selection is resolved with the same three-level tie-break chain:
 *
 *     1. the algorithm's own key   (burst / remaining / priority / arrival)
 *     2. earlier arrival time
 *     3. earlier position in the input order
 *
 * Round Robin does not use the chain for selection (its ready queue is
 * strictly FIFO) but it does use rule 2 then 3 to order processes that
 * arrive at the same instant.
 */
#ifndef SCHEDULIFY_ALGORITHMS_H
#define SCHEDULIFY_ALGORITHMS_H

#include <stdbool.h>
#include <stddef.h>

#include "process.h"

typedef struct {
    AlgorithmId id;
    const char *abbrev;      /* "SRTF"                                     */
    const char *name;        /* "Shortest Remaining Time First"            */
    const char *kind;        /* "preemptive" / "non-preemptive"            */
    bool        preemptive;
    bool        needs_quantum;
    const char *summary;     /* one line, shown in menus                   */
    const char *rule;        /* the exact selection + tie-break rule       */
} AlgorithmInfo;

const AlgorithmInfo *algo_info(AlgorithmId id);
const char *algo_name(AlgorithmId id);
const char *algo_abbrev(AlgorithmId id);
bool        algo_needs_quantum(AlgorithmId id);

/* Maps a short name ("fcfs", "srtf", "prio-p", "rr", ...) onto an id.
 * Shared by the command line and the web API so both accept the same
 * spellings. Case-insensitive. */
bool        algo_from_name(const char *name, AlgorithmId *out);

/* Individual schedulers.  Return false only on allocation failure. */
bool sched_fcfs(Schedule *sch);
bool sched_sjf(Schedule *sch);          /* non-preemptive, shortest burst  */
bool sched_srtf(Schedule *sch);         /* preemptive shortest remaining   */
bool sched_priority_np(Schedule *sch);  /* non-preemptive priority         */
bool sched_priority_p(Schedule *sch);   /* preemptive priority             */
bool sched_round_robin(Schedule *sch);  /* FIFO queue + time quantum       */

/* One-stop entry point: initialises sch from ds, runs the algorithm and
 * finalises the metrics.  On failure, writes a message into err and leaves
 * sch safe to pass to schedule_free().  quantum is ignored unless the
 * algorithm needs one. */
bool algo_run(Schedule *sch, const Dataset *ds, AlgorithmId algo, int quantum,
              bool enable_trace, char *err, size_t errsz);

#endif /* SCHEDULIFY_ALGORITHMS_H */
