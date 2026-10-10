/*
 * test_util.h -- A very small assertion framework.
 *
 * Every expectation in the suites is a number worked out by hand; the helpers
 * here only compare and report.  A timeline is written as a compact string so
 * an expected schedule reads the way it would be drawn on paper:
 *
 *     "P1:0-2,P2:2-4,IDLE:4-7,P2:7-9"
 */
#ifndef SCHEDULIFY_TEST_UTIL_H
#define SCHEDULIFY_TEST_UTIL_H

#include <stdbool.h>

#include "process.h"

typedef struct {
    const char *id;
    int arrival;
    int burst;
    int priority;
} PSpec;

/* ---- framework ---- */
void t_suite(const char *name);
void t_case(const char *name);
void t_check(bool ok, const char *file, int line, const char *fmt, ...);
void t_check_int(long got, long want, const char *file, int line,
                 const char *what);
void t_check_dbl(double got, double want, const char *file, int line,
                 const char *what);
void t_check_str(const char *got, const char *want, const char *file,
                 int line, const char *what);
int  t_total(void);
int  t_failed(void);
int  t_report(void);

#define CHECK(cond) \
    t_check((cond), __FILE__, __LINE__, "%s", #cond)
#define CHECK_MSG(cond, ...) \
    t_check((cond), __FILE__, __LINE__, __VA_ARGS__)
#define CHECK_INT(got, want) \
    t_check_int((long)(got), (long)(want), __FILE__, __LINE__, #got)
#define CHECK_DBL(got, want) \
    t_check_dbl((double)(got), (double)(want), __FILE__, __LINE__, #got)
#define CHECK_STR(got, want) \
    t_check_str((got), (want), __FILE__, __LINE__, #got)

/* ---- helpers ---- */

/* Builds a dataset from a literal table. */
void t_build(Dataset *ds, const char *name, const PSpec *spec, int n);

/* Renders a schedule's timeline as "P1:0-2,IDLE:2-5,P2:5-6". */
void t_timeline(const Schedule *sch, char *buf, size_t sz);

/* Index of the process with the given id, or -1. */
int t_find(const Schedule *sch, const char *id);

/* Runs one algorithm and asserts the whole timeline in one call.  Returns
 * false (after reporting) when the run itself failed. */
bool t_run(Schedule *sch, const Dataset *ds, AlgorithmId algo, int quantum,
           const char *file, int line);

#define RUN(sch, ds, algo, q) t_run((sch), (ds), (algo), (q), __FILE__, \
                                    __LINE__)

/* Convenience: per-process expectations. */
void t_check_proc(const Schedule *sch, const char *id, int completion,
                  int turnaround, int waiting, int response,
                  const char *file, int line);

#define CHECK_PROC(sch, id, ct, tat, wt, rt) \
    t_check_proc((sch), (id), (ct), (tat), (wt), (rt), __FILE__, __LINE__)

#endif /* SCHEDULIFY_TEST_UTIL_H */
