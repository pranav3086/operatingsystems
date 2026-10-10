/*
 * compare.c -- All six algorithms on one dataset, side by side.
 */
#include "compare.h"

#include <stdio.h>
#include <string.h>

#include "algorithms.h"
#include "table.h"
#include "ui.h"
#include "visualize.h"

bool compare_run(Comparison *cmp, const Dataset *ds, int quantum,
                 char *err, size_t errsz)
{
    int i;

    memset(cmp, 0, sizeof *cmp);
    cmp->quantum = quantum;
    cmp->n = ds->count;
    snprintf(cmp->dataset_name, sizeof cmp->dataset_name, "%s", ds->name);

    for (i = 0; i < ALGO_COUNT; i++) {
        cmp->ok[i] = algo_run(&cmp->sch[i], ds, (AlgorithmId)i, quantum,
                              false, cmp->err[i], sizeof cmp->err[i]);
    }

    for (i = 0; i < ALGO_COUNT; i++) {
        if (cmp->ok[i]) return true;
    }
    snprintf(err, errsz, "%s", cmp->err[0]);
    return false;
}

void compare_free(Comparison *cmp)
{
    int i;
    for (i = 0; i < ALGO_COUNT; i++) schedule_free(&cmp->sch[i]);
}

/* ------------------------------------------------------------ winners ---- */

typedef enum { LOWER_IS_BETTER, HIGHER_IS_BETTER } Direction;

static int best_index(const Comparison *cmp, double (*get)(const Schedule *),
                      Direction dir)
{
    int i, best = -1;
    for (i = 0; i < ALGO_COUNT; i++) {
        double v;
        if (!cmp->ok[i]) continue;
        v = get(&cmp->sch[i]);
        if (best < 0) { best = i; continue; }
        if (dir == LOWER_IS_BETTER) {
            if (v < get(&cmp->sch[best]) - 1e-9) best = i;
        } else {
            if (v > get(&cmp->sch[best]) + 1e-9) best = i;
        }
    }
    return best;
}

static double g_tat(const Schedule *s)   { return s->avg_turnaround; }
static double g_wt(const Schedule *s)    { return s->avg_waiting; }
static double g_rt(const Schedule *s)    { return s->avg_response; }
static double g_util(const Schedule *s)  { return s->cpu_utilization; }
static double g_thru(const Schedule *s)  { return s->throughput; }
static double g_ctx(const Schedule *s)   { return (double)s->context_switches; }
static double g_pre(const Schedule *s)   { return (double)s->preemptions; }
static double g_span(const Schedule *s)  { return (double)s->total_time; }

/* The best value in a column, or NAN when nothing could be simulated. */
static double best_value(const Comparison *cmp,
                         double (*get)(const Schedule *), Direction dir)
{
    int b = best_index(cmp, get, dir);
    if (b < 0) return 0.0;
    return get(&cmp->sch[b]);
}

static bool ties_with(double v, double best)
{
    return v > best - 1e-9 && v < best + 1e-9;
}

/* Appends " *" to every value that ties for the best in its column, not
 * just to the first one found. */
static void cell_num(Table *t, double v, int digits, double best)
{
    const char *mark = ties_with(v, best) ? " *" : "";
    if (digits == 0)      tbl_cellf(t, "%.0f%s", v, mark);
    else if (digits == 2) tbl_cellf(t, "%.2f%s", v, mark);
    else                  tbl_cellf(t, "%.4f%s", v, mark);
}

void compare_table(const Comparison *cmp)
{
    static const char *const H[] = { "Algorithm", "Avg TAT", "Avg WT",
                                     "Avg RT", "Util %", "Thru", "Ctx",
                                     "Pre", "Span", "Idle" };
    static const TblAlign A[] = { TBL_LEFT, TBL_RIGHT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT, TBL_RIGHT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT, TBL_RIGHT };
    Table t;
    int i;
    double v_tat  = best_value(cmp, g_tat,  LOWER_IS_BETTER);
    double v_wt   = best_value(cmp, g_wt,   LOWER_IS_BETTER);
    double v_rt   = best_value(cmp, g_rt,   LOWER_IS_BETTER);
    double v_util = best_value(cmp, g_util, HIGHER_IS_BETTER);
    double v_thru = best_value(cmp, g_thru, HIGHER_IS_BETTER);
    double v_ctx  = best_value(cmp, g_ctx,  LOWER_IS_BETTER);
    double v_pre  = best_value(cmp, g_pre,  LOWER_IS_BETTER);
    double v_span = best_value(cmp, g_span, LOWER_IS_BETTER);

    ui_section("Comparison on \"%s\"  (%d process%s)",
               cmp->dataset_name, cmp->n, (cmp->n == 1) ? "" : "es");

    tbl_init(&t, 10, H, A);
    for (i = 0; i < ALGO_COUNT; i++) {
        const Schedule *s = &cmp->sch[i];
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);

        tbl_row(&t);
        if (info->needs_quantum) {
            tbl_cellf(&t, "%s q=%d", info->abbrev, cmp->quantum);
        } else {
            tbl_cellf(&t, "%s", info->abbrev);
        }
        if (!cmp->ok[i]) {
            int c;
            tbl_cellf(&t, "failed");
            for (c = 2; c < 10; c++) tbl_cellf(&t, "-");
            continue;
        }
        cell_num(&t, s->avg_turnaround, 2, v_tat);
        cell_num(&t, s->avg_waiting, 2, v_wt);
        cell_num(&t, s->avg_response, 2, v_rt);
        cell_num(&t, s->cpu_utilization, 2, v_util);
        cell_num(&t, s->throughput, 4, v_thru);
        cell_num(&t, (double)s->context_switches, 0, v_ctx);
        cell_num(&t, (double)s->preemptions, 0, v_pre);
        cell_num(&t, (double)s->total_time, 0, v_span);
        tbl_cellf(&t, "%d", s->idle_time);
    }
    tbl_render(&t, 2);
    tbl_free(&t);

    ui_wrap_dim("* marks the best value in the column (every algorithm that "
                "ties for it). TAT = turnaround, WT = waiting, "
                "RT = response, Thru = throughput, Ctx = context switches, "
                "Pre = preemptions, Span = makespan (elapsed time of the "
                "whole schedule).", 2);
    ui_wrap_dim("Makespan, utilization and idle time come out the same for "
                "every work-conserving algorithm on a workload with no idle "
                "CPU -- the differences live in the averages and in the "
                "switch counts.", 2);

    for (i = 0; i < ALGO_COUNT; i++) {
        if (!cmp->ok[i])
            ui_error("%s could not be simulated: %s",
                     algo_abbrev((AlgorithmId)i), cmp->err[i]);
    }
}

void compare_charts(const Comparison *cmp)
{
    int i;
    for (i = 0; i < ALGO_COUNT; i++) {
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);
        if (!cmp->ok[i]) continue;
        if (info->needs_quantum) {
            ui_section("%s -- %s (quantum = %d)", info->abbrev, info->name,
                       cmp->quantum);
        } else {
            ui_section("%s -- %s (%s)", info->abbrev, info->name, info->kind);
        }
        viz_gantt(&cmp->sch[i]);
    }
}

void compare_verdict(const Comparison *cmp)
{
    struct { const char *label; double (*get)(const Schedule *);
             Direction dir; int digits; const char *unit; } crit[] = {
        { "Lowest average turnaround time", g_tat,  LOWER_IS_BETTER,  2, "" },
        { "Lowest average waiting time",    g_wt,   LOWER_IS_BETTER,  2, "" },
        { "Lowest average response time",   g_rt,   LOWER_IS_BETTER,  2, "" },
        { "Highest CPU utilization",        g_util, HIGHER_IS_BETTER, 2, " %" },
        { "Highest throughput",             g_thru, HIGHER_IS_BETTER, 4,
          " proc/unit" },
        { "Fewest context switches",        g_ctx,  LOWER_IS_BETTER,  0, "" },
        { "Fewest preemptions",             g_pre,  LOWER_IS_BETTER,  0, "" }
    };
    const int ncrit = (int)(sizeof crit / sizeof crit[0]);
    int c;

    ui_section("Verdict");
    for (c = 0; c < ncrit; c++) {
        int b = best_index(cmp, crit[c].get, crit[c].dir);
        char tied[160];
        int i, ntied = 0;

        if (b < 0) continue;
        tied[0] = '\0';
        for (i = 0; i < ALGO_COUNT; i++) {
            double v;
            if (!cmp->ok[i]) continue;
            v = crit[c].get(&cmp->sch[i]);
            if (v > crit[c].get(&cmp->sch[b]) - 1e-9 &&
                v < crit[c].get(&cmp->sch[b]) + 1e-9) {
                size_t len = strlen(tied);
                snprintf(tied + len, sizeof tied - len, "%s%s",
                         (ntied > 0) ? ", " : "",
                         algo_abbrev((AlgorithmId)i));
                ntied++;
            }
        }
        printf("  %s%-32s%s %s%s%s  %s(%.*f%s)%s\n",
               ui_c(C_LABEL), crit[c].label, ui_c(C_RESET),
               ui_c(C_OK), tied, ui_c(C_RESET),
               ui_c(C_DIM), crit[c].digits,
               crit[c].get(&cmp->sch[b]), crit[c].unit, ui_c(C_RESET));
    }
    printf("\n");
    ui_wrap_dim("A single dataset does not make one algorithm better than "
                "another: SJF and SRTF minimise average waiting time but "
                "starve long jobs, Round Robin bounds response time at the "
                "cost of extra switches, and FCFS is fair in arrival order "
                "but suffers the convoy effect.", 2);
}

void compare_report(const Comparison *cmp)
{
    ui_title("Comparison of all %d algorithms", ALGO_COUNT);
    compare_charts(cmp);
    compare_table(cmp);
    compare_verdict(cmp);
}
