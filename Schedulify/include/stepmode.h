/*
 * stepmode.h -- The step-by-step educational mode.
 *
 * The algorithm is run once with decision tracing enabled, then the recorded
 * decisions are replayed one at a time.  At every step the user sees:
 *
 *   - the scheduler's own explanation of what it is about to do and why,
 *     including which other processes were ready and what the tie-break
 *     rule decided;
 *   - the Gantt chart as it stands at that instant, growing as the
 *     simulation advances;
 *   - a state panel with each process's arrival status, executed time and
 *     remaining time.
 *
 * Nothing here recomputes the schedule: the replay is driven entirely by the
 * timeline and trace that the real algorithm produced.
 */
#ifndef SCHEDULIFY_STEPMODE_H
#define SCHEDULIFY_STEPMODE_H

#include "process.h"
#include "validate.h"

/* Walks the user through one algorithm on one dataset.  Returns PROMPT_EOF
 * when stdin is exhausted so the caller can shut down cleanly. */
PromptResult step_mode_run(const Dataset *ds, AlgorithmId algo, int quantum);

#endif /* SCHEDULIFY_STEPMODE_H */
