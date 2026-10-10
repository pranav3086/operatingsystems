/*
 * datasets.h -- Predefined teaching examples and the random generator.
 *
 * The examples are the ones whose answers are worked out by hand in the
 * README and checked by the unit tests, so a student can compare what
 * Schedulify prints against pencil and paper.
 *
 * The random generator uses its own xorshift PRNG rather than rand(), so a
 * given seed produces the same workload on every platform and compiler.
 */
#ifndef SCHEDULIFY_DATASETS_H
#define SCHEDULIFY_DATASETS_H

#include <stdbool.h>

#include "process.h"

int         examples_count(void);
const char *example_name(int index);
const char *example_description(int index);

/* The time quantum that makes the example interesting (always valid). */
int         example_quantum(int index);

/* Initialises ds and fills it with the example.  Returns false on a bad
 * index or on allocation failure. */
bool        example_load(int index, Dataset *ds);

/* Random workload generator.  n processes, arrival in [0,max_arrival],
 * burst in [1,max_burst], priority in [0,max_priority].  ds is initialised
 * by this call.  The same seed always yields the same dataset. */
bool dataset_random(Dataset *ds, int n, unsigned long seed, int max_arrival,
                    int max_burst, int max_priority);

/* A seed derived from the clock, for when the user asks for "surprise me". */
unsigned long dataset_random_seed(void);

#endif /* SCHEDULIFY_DATASETS_H */
