/*
 * visualize.h -- Terminal rendering of datasets, Gantt charts and metrics.
 *
 * The Gantt chart is drawn to scale: a cell's width is proportional to the
 * duration of its segment, with a per-cell minimum so the process label
 * always fits.  The ruler underneath carries the exact boundary times, so
 * the chart stays accurate even where a one-unit slice had to be widened to
 * hold its label.  Charts wider than the terminal wrap onto further rows
 * rather than being squashed.
 */
#ifndef SCHEDULIFY_VISUALIZE_H
#define SCHEDULIFY_VISUALIZE_H

#include "process.h"

/* Input workload, as entered. */
void viz_dataset_table(const Dataset *ds);

/* Per-process results: arrival, burst, priority, start, completion,
 * turnaround, waiting, response, plus an averages row. */
void viz_process_table(const Schedule *sch);

/* The full Gantt chart. */
void viz_gantt(const Schedule *sch);

/* The Gantt chart clipped to [window_start, t_limit) -- used by the
 * step-by-step mode to grow the chart one decision at a time. */
void viz_gantt_upto(const Schedule *sch, int t_limit);

/* Exact segment boundaries as a list, for long schedules and for checking
 * the chart by hand. */
void viz_segment_list(const Schedule *sch);

/* Aggregate metrics block. */
void viz_summary(const Schedule *sch);

/* Heading + Gantt chart + segment list + process table + summary. */
void viz_report(const Schedule *sch);

/* The documented scheduling assumptions and metric definitions. */
void viz_assumptions(void);

/* One paragraph per algorithm: what it does and how ties are broken. */
void viz_algorithm_rules(void);

#endif /* SCHEDULIFY_VISUALIZE_H */
