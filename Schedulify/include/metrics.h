/*
 * metrics.h -- Derives every reported number from the execution timeline.
 *
 * The scheduling algorithms write Segments; this module turns those Segments
 * into per-process and aggregate metrics.  All six algorithms therefore share
 * one definition of each metric, which is what makes the comparison table
 * meaningful.
 *
 * DEFINITIONS (also printed by the "Scheduling assumptions" menu entry)
 * --------------------------------------------------------------------
 *  observation window   [earliest arrival, last completion].  Time before any
 *                       process exists is not charged to the CPU.
 *  completion time      end of a process's last segment.
 *  turnaround time      completion - arrival.
 *  waiting time         turnaround - burst  (equivalently, time spent ready
 *                       but not running).
 *  response time        first time it got the CPU - arrival.
 *  idle time            total length of segments in which no process ran,
 *                       inside the observation window.
 *  busy time            window length - idle time == sum of all bursts.
 *  CPU utilization      100 * busy / window length.
 *  throughput           completed processes / window length.
 *  context switches     number of times the CPU is handed from one process to
 *                       a *different* process.  Adjacent slices of the same
 *                       process are merged first, so a Round Robin quantum
 *                       that expires with no other process ready costs
 *                       nothing.  The first dispatch is not a switch.
 *  preemptions          number of times a process leaves the CPU while it
 *                       still has work left.  A process that runs out its
 *                       quantum and is immediately re-dispatched (because no
 *                       one else was ready) is not preempted.
 */
#ifndef SCHEDULIFY_METRICS_H
#define SCHEDULIFY_METRICS_H

#include <stdbool.h>

#include "process.h"

/* Fills sch->stats[] and every aggregate field.  Safe to call on an empty
 * schedule.  Returns false if the timeline is inconsistent with the input
 * (a process that never ran, or executed more/less than its burst), which
 * would indicate a bug in an algorithm rather than bad user input. */
bool metrics_finalize(Schedule *sch);

/* Human-readable reason the last metrics_finalize() rejected a timeline. */
const char *metrics_last_error(void);

/* Index of the process with the smallest/largest value of a metric, used by
 * the comparison module to call out winners.  Returns -1 when n == 0. */
int metrics_best_process_by_turnaround(const Schedule *sch);

#endif /* SCHEDULIFY_METRICS_H */
