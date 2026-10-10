/*
 * stepmode.c -- Replay of a traced run, one decision at a time.
 */
#include "stepmode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "table.h"
#include "ui.h"
#include "visualize.h"

/* CPU time granted to each process strictly before time t. */
static void executed_before(const Schedule *sch, int t, int *executed)
{
    int i;
    for (i = 0; i < sch->n; i++) executed[i] = 0;
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        int end;
        if (s->proc == IDLE_INDEX) continue;
        if (s->start >= t) break;
        end = (s->end < t) ? s->end : t;
        executed[s->proc] += end - s->start;
    }
}

/* The process that held the CPU during the unit ending at t, or -1. */
static int last_on_cpu(const Schedule *sch, int t)
{
    int i;
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        if (s->start < t && t <= s->end) return s->proc;
    }
    return -1;
}

static void state_panel(const Schedule *sch, int t, const int *executed)
{
    static const char *const H[] = { "Process", "Arrival", "Burst",
                                     "Executed", "Remaining", "State" };
    static const TblAlign A[] = { TBL_LEFT, TBL_RIGHT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT, TBL_LEFT };
    Table tb;
    int i;
    int on_cpu = last_on_cpu(sch, t);

    tbl_init(&tb, 6, H, A);
    for (i = 0; i < sch->n; i++) {
        const Process *p = &sch->procs[i];
        int rem = p->burst - executed[i];
        const char *state;

        if (rem == 0)                   state = "finished";
        else if (p->arrival > t)        state = "not arrived";
        else if (i == on_cpu)           state = "ready (was on CPU)";
        else                            state = "ready";

        tbl_row(&tb);
        tbl_cellf(&tb, "%s", p->id);
        tbl_cellf(&tb, "%d", p->arrival);
        tbl_cellf(&tb, "%d", p->burst);
        tbl_cellf(&tb, "%d", executed[i]);
        tbl_cellf(&tb, "%d", rem);
        tbl_cellf(&tb, "%s", state);
    }
    tbl_render(&tb, 2);
    tbl_free(&tb);
}

PromptResult step_mode_run(const Dataset *ds, AlgorithmId algo, int quantum)
{
    Schedule sch;
    char err[ERR_MSG_MAX];
    const AlgorithmInfo *info = algo_info(algo);
    int *executed = NULL;
    int step = 0;
    bool autoplay = false;
    PromptResult pr = PROMPT_OK;

    if (!algo_run(&sch, ds, algo, quantum, true, err, sizeof err)) {
        ui_error("%s", err);
        schedule_free(&sch);
        return PROMPT_OK;
    }

    if (sch.trace.count == 0) {
        ui_warn("this run produced no decisions to step through");
        schedule_free(&sch);
        return PROMPT_OK;
    }

    executed = malloc((size_t)(sch.n > 0 ? sch.n : 1) * sizeof *executed);
    if (executed == NULL) {
        ui_error("out of memory");
        schedule_free(&sch);
        return PROMPT_OK;
    }

    ui_clear_screen();
    if (info->needs_quantum) {
        ui_title("Step-by-step: %s (quantum = %d)", info->name, quantum);
    } else {
        ui_title("Step-by-step: %s (%s)", info->name, info->kind);
    }
    viz_dataset_table(ds);

    while (step < sch.trace.count) {
        const TraceEvent *ev = &sch.trace.items[step];

        ui_rule();
        printf("  %sstep %d of %d%s   %sclock t=%d%s\n",
               ui_c(C_TITLE), step + 1, sch.trace.count, ui_c(C_RESET),
               ui_c(C_LABEL), ev->time, ui_c(C_RESET));
        printf("\n");
        ui_wrap(ev->text, 2);   /* the scheduler's own explanation */

        executed_before(&sch, ev->time, executed);
        viz_gantt_upto(&sch, ev->time);
        printf("\n");
        state_panel(&sch, ev->time, executed);

        step++;

        if (autoplay) continue;
        if (step >= sch.trace.count) break;

        for (;;) {
            char buf[32];
            ui_prompt("\n  [Enter] next   [b] back   [a] all   [q] stop: ");
            pr = read_line(buf, sizeof buf);
            if (pr == PROMPT_EOF) goto done;
            trim(buf);
            if (buf[0] == '\0') break;
            if (buf[0] == 'a' || buf[0] == 'A') { autoplay = true; break; }
            if (buf[0] == 'q' || buf[0] == 'Q') { step = sch.trace.count;
                                                  goto finish; }
            if (buf[0] == 'b' || buf[0] == 'B') {
                if (step >= 2) step -= 2;
                else step = 0;
                break;
            }
            ui_error("press Enter, or b, a or q");
        }
    }

finish:
    ui_rule();
    ui_success("replay complete -- here is the finished schedule");
    viz_report(&sch);

done:
    free(executed);
    schedule_free(&sch);
    return (pr == PROMPT_EOF) ? PROMPT_EOF : PROMPT_OK;
}
