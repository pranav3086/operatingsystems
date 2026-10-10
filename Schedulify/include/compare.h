/*
 * compare.h -- Runs every algorithm on one dataset and tabulates the result.
 *
 * All six schedules are kept alive at the same time so the comparison can
 * show the charts next to the numbers, and so the CSV exporter can write
 * them all from one run.
 */
#ifndef SCHEDULIFY_COMPARE_H
#define SCHEDULIFY_COMPARE_H

#include <stdbool.h>

#include "process.h"

#define COMPARE_ERR_MAX 160

typedef struct {
    Schedule sch[ALGO_COUNT];
    bool     ok[ALGO_COUNT];
    char     err[ALGO_COUNT][COMPARE_ERR_MAX];
    int      quantum;
    char     dataset_name[DATASET_NAME_MAX];
    int      n;
} Comparison;

/* Runs all six algorithms.  Returns false only when the dataset itself is
 * rejected; individual failures are recorded in ok[]/err[].  Always pair a
 * true return with compare_free(). */
bool compare_run(Comparison *cmp, const Dataset *ds, int quantum,
                 char *err, size_t errsz);

void compare_free(Comparison *cmp);

/* Side-by-side metric table with the best value in each column marked. */
void compare_table(const Comparison *cmp);

/* Every Gantt chart, one after the other. */
void compare_charts(const Comparison *cmp);

/* Plain-language summary of which algorithm won which criterion. */
void compare_verdict(const Comparison *cmp);

/* Heading + charts + table + verdict. */
void compare_report(const Comparison *cmp);

#endif /* SCHEDULIFY_COMPARE_H */
