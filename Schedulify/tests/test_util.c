/*
 * test_util.c -- The assertion framework and dataset/schedule helpers.
 */
#include "test_util.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "validate.h"

static int g_total = 0;
static int g_failed = 0;
static const char *g_suite = "";
static const char *g_case = "";

void t_suite(const char *name)
{
    g_suite = name;
    g_case = "";
    printf("\n== %s ==\n", name);
}

void t_case(const char *name)
{
    g_case = name;
    printf("  -- %s\n", name);
}

void t_check(bool ok, const char *file, int line, const char *fmt, ...)
{
    g_total++;
    if (ok) return;
    g_failed++;
    printf("     FAIL  %s:%d  [%s / %s]  ", file, line, g_suite, g_case);
    {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
}

void t_check_int(long got, long want, const char *file, int line,
                 const char *what)
{
    g_total++;
    if (got == want) return;
    g_failed++;
    printf("     FAIL  %s:%d  [%s / %s]  %s: got %ld, expected %ld\n",
           file, line, g_suite, g_case, what, got, want);
}

void t_check_dbl(double got, double want, const char *file, int line,
                 const char *what)
{
    g_total++;
    if (fabs(got - want) < 1e-6) return;
    g_failed++;
    printf("     FAIL  %s:%d  [%s / %s]  %s: got %.6f, expected %.6f\n",
           file, line, g_suite, g_case, what, got, want);
}

void t_check_str(const char *got, const char *want, const char *file,
                 int line, const char *what)
{
    g_total++;
    if (got != NULL && want != NULL && strcmp(got, want) == 0) return;
    g_failed++;
    printf("     FAIL  %s:%d  [%s / %s]  %s:\n"
           "             got      \"%s\"\n"
           "             expected \"%s\"\n",
           file, line, g_suite, g_case, what,
           (got != NULL) ? got : "(null)",
           (want != NULL) ? want : "(null)");
}

int t_total(void)  { return g_total; }
int t_failed(void) { return g_failed; }

int t_report(void)
{
    printf("\n--------------------------------------------------\n");
    if (g_failed == 0) {
        printf("All %d checks passed.\n", g_total);
        return 0;
    }
    printf("%d of %d checks FAILED.\n", g_failed, g_total);
    return 1;
}

/* ----------------------------------------------------------- helpers --- */

void t_build(Dataset *ds, const char *name, const PSpec *spec, int n)
{
    int i;
    dataset_init(ds, name);
    for (i = 0; i < n; i++) {
        Process p;
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "%s", spec[i].id);
        p.arrival = spec[i].arrival;
        p.burst = spec[i].burst;
        p.priority = spec[i].priority;
        if (!dataset_add(ds, &p)) {
            printf("     FATAL  could not add %s to dataset\n", spec[i].id);
            exit(2);
        }
    }
}

void t_timeline(const Schedule *sch, char *buf, size_t sz)
{
    int i;
    size_t used = 0;

    if (sz == 0) return;
    buf[0] = '\0';
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        const char *id = (s->proc == IDLE_INDEX) ? "IDLE"
                                                 : sch->procs[s->proc].id;
        int len = snprintf(buf + used, sz - used, "%s%s:%d-%d",
                           (i > 0) ? "," : "", id, s->start, s->end);
        if (len < 0 || (size_t)len >= sz - used) return;
        used += (size_t)len;
    }
}

int t_find(const Schedule *sch, const char *id)
{
    int i;
    for (i = 0; i < sch->n; i++) {
        if (strcmp(sch->procs[i].id, id) == 0) return i;
    }
    return -1;
}

bool t_run(Schedule *sch, const Dataset *ds, AlgorithmId algo, int quantum,
           const char *file, int line)
{
    char err[ERR_MSG_MAX];
    g_total++;
    if (algo_run(sch, ds, algo, quantum, false, err, sizeof err)) return true;
    g_failed++;
    printf("     FAIL  %s:%d  [%s / %s]  %s could not run: %s\n",
           file, line, g_suite, g_case, algo_abbrev(algo), err);
    return false;
}

void t_check_proc(const Schedule *sch, const char *id, int completion,
                  int turnaround, int waiting, int response,
                  const char *file, int line)
{
    int i = t_find(sch, id);
    char what[64];

    if (i < 0) {
        g_total++;
        g_failed++;
        printf("     FAIL  %s:%d  [%s / %s]  no process \"%s\"\n",
               file, line, g_suite, g_case, id);
        return;
    }
    snprintf(what, sizeof what, "%s completion", id);
    t_check_int(sch->stats[i].completion, completion, file, line, what);
    snprintf(what, sizeof what, "%s turnaround", id);
    t_check_int(sch->stats[i].turnaround, turnaround, file, line, what);
    snprintf(what, sizeof what, "%s waiting", id);
    t_check_int(sch->stats[i].waiting, waiting, file, line, what);
    snprintf(what, sizeof what, "%s response", id);
    t_check_int(sch->stats[i].response, response, file, line, what);
}
