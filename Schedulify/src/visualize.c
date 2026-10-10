/*
 * visualize.c -- Dataset tables, Gantt charts, metric summaries, help text.
 */
#include "visualize.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "table.h"
#include "ui.h"

#define IDLE_LABEL "IDLE"

/* ------------------------------------------------------------- utilities */

static void pad(int n)
{
    int i;
    for (i = 0; i < n; i++) fputc(' ', stdout);
}

#define wrap_print(text, indent) ui_wrap((text), (indent))

/* "  Label ............. value", with the value wrapped under itself.  On a
 * narrow terminal the dot leader shrinks first so the value keeps its room. */
static void kv(const char *label, const char *fmt, ...)
{
    char val[192];
    va_list ap;
    int label_len = (int)strlen(label);
    int leader = 32;
    int dots, col;

    va_start(ap, fmt);
    vsnprintf(val, sizeof val, fmt, ap);
    va_end(ap);

    if (leader > ui_term_width() - 28) leader = ui_term_width() - 28;
    if (leader < label_len + 2) leader = label_len + 2;
    dots = leader - label_len;

    printf("  %s%s%s ", ui_c(C_LABEL), label, ui_c(C_RESET));
    fputs(ui_c(C_DIM), stdout);
    ui_repeat(".", dots);
    fputs(ui_c(C_RESET), stdout);
    fputc(' ', stdout);

    col = 2 + label_len + 1 + dots + 1;
    fputs(ui_c(C_VALUE), stdout);
    ui_wrap_at(val, col, col);
    printf("%s\n", ui_c(C_RESET));
}

/* ---------------------------------------------------------- input table */

void viz_dataset_table(const Dataset *ds)
{
    static const char *const H[] = { "#", "Process", "Arrival", "Burst",
                                     "Priority" };
    static const TblAlign A[] = { TBL_RIGHT, TBL_LEFT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT };
    Table t;
    int i;

    ui_section("Workload: %s  (%d process%s)", ds->name, ds->count,
               (ds->count == 1) ? "" : "es");

    if (ds->count == 0) {
        ui_note("the dataset is empty");
        return;
    }

    tbl_init(&t, 5, H, A);
    for (i = 0; i < ds->count; i++) {
        tbl_row(&t);
        tbl_cellf(&t, "%d", i + 1);
        tbl_cellf(&t, "%s", ds->items[i].id);
        tbl_cellf(&t, "%d", ds->items[i].arrival);
        tbl_cellf(&t, "%d", ds->items[i].burst);
        tbl_cellf(&t, "%d", ds->items[i].priority);
    }
    tbl_render(&t, 2);
    tbl_free(&t);

    printf("  %stotal CPU demand %d unit(s), earliest arrival t=%d%s\n",
           ui_c(C_DIM), dataset_total_burst(ds), dataset_min_arrival(ds),
           ui_c(C_RESET));
    printf("  %spriority convention: lower number == higher priority%s\n",
           ui_c(C_DIM), ui_c(C_RESET));
}

/* -------------------------------------------------------- results table */

void viz_process_table(const Schedule *sch)
{
    static const char *const H[] = { "Process", "Arrival", "Burst", "Prio",
                                     "Start", "Finish", "Turnaround",
                                     "Waiting", "Response" };
    static const TblAlign A[] = { TBL_LEFT, TBL_RIGHT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT, TBL_RIGHT, TBL_RIGHT, TBL_RIGHT,
                                  TBL_RIGHT };
    Table t;
    int i;

    ui_section("Process table");

    if (sch->n == 0) {
        ui_note("nothing to show");
        return;
    }

    tbl_init(&t, 9, H, A);
    for (i = 0; i < sch->n; i++) {
        const Process *p = &sch->procs[i];
        const ProcStats *s = &sch->stats[i];
        tbl_row(&t);
        tbl_cellf(&t, "%s", p->id);
        tbl_cellf(&t, "%d", p->arrival);
        tbl_cellf(&t, "%d", p->burst);
        tbl_cellf(&t, "%d", p->priority);
        tbl_cellf(&t, "%d", s->first_start);
        tbl_cellf(&t, "%d", s->completion);
        tbl_cellf(&t, "%d", s->turnaround);
        tbl_cellf(&t, "%d", s->waiting);
        tbl_cellf(&t, "%d", s->response);
    }
    tbl_separator(&t);
    tbl_row_emphasis(&t);
    tbl_cellf(&t, "AVERAGE");
    tbl_cellf(&t, "-");
    tbl_cellf(&t, "-");
    tbl_cellf(&t, "-");
    tbl_cellf(&t, "-");
    tbl_cellf(&t, "%.2f", sch->avg_completion);
    tbl_cellf(&t, "%.2f", sch->avg_turnaround);
    tbl_cellf(&t, "%.2f", sch->avg_waiting);
    tbl_cellf(&t, "%.2f", sch->avg_response);

    tbl_render(&t, 2);
    tbl_free(&t);

    ui_wrap_dim("Start = first time the process held the CPU;  "
                "Turnaround = Finish - Arrival;  "
                "Waiting = Turnaround - Burst;  "
                "Response = Start - Arrival.", 2);
}

/* ------------------------------------------------------------ Gantt chart */

typedef struct {
    int proc;
    int start;
    int end;
    int width;
    const char *label;
} GCell;

/* The narrowest a cell may be: wide enough for its label, and wide enough
 * that its ruler number cannot run into the next one.  No decorative
 * padding is added, so a cell is only ever widened when it genuinely has
 * to be -- that keeps the chart to scale as often as possible. */
static int min_cell_width(const GCell *c)
{
    char num[16];
    int lw = ui_display_width(c->label);
    int nw;
    snprintf(num, sizeof num, "%d", c->start);
    nw = (int)strlen(num) + 1;
    if (lw < nw) lw = nw;
    if (lw < 2) lw = 2;
    return lw;
}

static void ruler_put(char *buf, size_t sz, int col, int value, int *next_free)
{
    char num[16];
    int len = snprintf(num, sizeof num, "%d", value);

    if (len <= 0) return;
    if (col < *next_free) col = *next_free;
    if ((size_t)col + (size_t)len >= sz) return;
    memcpy(buf + col, num, (size_t)len);
    *next_free = col + len + 1;
}

static void draw_chunk(const GCell *cells, int from, int to, int indent)
{
    const BoxChars *b = ui_box();
    int total = 1;
    int k;
    char *ruler;
    int next_free = 0;
    int col;

    for (k = from; k < to; k++) total += cells[k].width + 1;

    /* top border */
    pad(indent);
    fputs(ui_c(C_DIM), stdout);
    fputs(b->tl, stdout);
    for (k = from; k < to; k++) {
        ui_repeat(b->h, cells[k].width);
        fputs((k + 1 < to) ? b->t_down : b->tr, stdout);
    }
    printf("%s\n", ui_c(C_RESET));

    /* labels */
    pad(indent);
    printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    for (k = from; k < to; k++) {
        int w = cells[k].width;
        int lw = ui_display_width(cells[k].label);
        int left = (w - lw) / 2;
        int right = w - lw - left;
        const char *color = ui_c(ui_proc_color(cells[k].proc));
        if (left < 0) left = 0;
        if (right < 0) right = 0;
        printf("%*s%s%s%s%*s", left, "", color, cells[k].label,
               ui_c(C_RESET), right, "");
        printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    }
    fputc('\n', stdout);

    /* bottom border */
    pad(indent);
    fputs(ui_c(C_DIM), stdout);
    fputs(b->bl, stdout);
    for (k = from; k < to; k++) {
        ui_repeat(b->h, cells[k].width);
        fputs((k + 1 < to) ? b->t_up : b->br, stdout);
    }
    printf("%s\n", ui_c(C_RESET));

    /* ruler */
    ruler = malloc((size_t)total + 16);
    if (ruler == NULL) return;
    memset(ruler, ' ', (size_t)total + 15);
    ruler[total + 15] = '\0';

    col = 0;
    for (k = from; k < to; k++) {
        ruler_put(ruler, (size_t)total + 16, col, cells[k].start, &next_free);
        col += cells[k].width + 1;
    }
    ruler_put(ruler, (size_t)total + 16, col, cells[to - 1].end, &next_free);

    {
        size_t len = strlen(ruler);
        while (len > 0 && ruler[len - 1] == ' ') ruler[--len] = '\0';
    }
    pad(indent);
    printf("%s%s%s\n", ui_c(C_LABEL), ruler, ui_c(C_RESET));
    free(ruler);
}

static void gantt_draw(const Schedule *sch, int t_limit)
{
    GCell *cells;
    int count = 0;
    int i, k, avail, scale, widened = 0;
    const int indent = 2;

    if (sch->seg_count == 0) {
        ui_note("the timeline is empty");
        return;
    }

    cells = malloc((size_t)sch->seg_count * sizeof *cells);
    if (cells == NULL) {
        ui_error("out of memory while drawing the Gantt chart");
        return;
    }

    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        int end = (s->end < t_limit) ? s->end : t_limit;
        if (s->start >= t_limit) break;
        cells[count].proc = s->proc;
        cells[count].start = s->start;
        cells[count].end = end;
        cells[count].label = (s->proc == IDLE_INDEX) ? IDLE_LABEL
                                                     : sch->procs[s->proc].id;
        count++;
    }

    if (count == 0) {
        ui_note("nothing has executed yet");
        free(cells);
        return;
    }

    avail = ui_term_width() - 2 * indent;
    if (avail < 40) avail = 40;

    /* Largest number of characters per time unit that still fits on one
     * row; fall back to 1 and wrap when even that is too wide. */
    scale = 1;
    for (k = 4; k >= 1; k--) {
        int total = 1;
        for (i = 0; i < count; i++) {
            int dur = cells[i].end - cells[i].start;
            int w = dur * k;
            int mw = min_cell_width(&cells[i]);
            if (w < mw) w = mw;
            total += w + 1;
        }
        if (total <= avail) { scale = k; break; }
    }

    for (i = 0; i < count; i++) {
        int dur = cells[i].end - cells[i].start;
        int mw = min_cell_width(&cells[i]);
        cells[i].width = dur * scale;
        if (cells[i].width < mw) { cells[i].width = mw; widened++; }
    }

    {
        static const char HEADING[] = "Gantt chart";
        char note[160];
        snprintf(note, sizeof note,
                 "(%d character%s per time unit; the ruler shows exact "
                 "boundaries)", scale, (scale == 1) ? "" : "s");
        printf("\n  %s%s%s ", ui_c(C_LABEL), HEADING, ui_c(C_RESET));
        fputs(ui_c(C_DIM), stdout);
        ui_wrap_at(note, indent, indent + (int)sizeof HEADING);
        printf("%s\n", ui_c(C_RESET));
    }

    k = 0;
    while (k < count) {
        int total = 1;
        int end = k;
        while (end < count) {
            int add = cells[end].width + 1;
            if (total + add > avail && end > k) break;
            total += add;
            end++;
        }
        if (k > 0) fputc('\n', stdout);
        draw_chunk(cells, k, end, indent);
        k = end;
    }

    if (widened > 0) {
        char note[200];
        snprintf(note, sizeof note,
                 "%d cell%s had to be widened so the label would fit, so "
                 "%s not drawn to scale; the ruler underneath is always "
                 "exact.", widened, (widened == 1) ? "" : "s",
                 (widened == 1) ? "it is" : "they are");
        ui_wrap_dim(note, 2);
    }
    if (sch->idle_time > 0) {
        printf("  %s%s marks a stretch where no process was ready to run.%s\n",
               ui_c(C_DIM), IDLE_LABEL, ui_c(C_RESET));
    }

    free(cells);
}

void viz_gantt(const Schedule *sch)
{
    gantt_draw(sch, INT_MAX);
}

void viz_gantt_upto(const Schedule *sch, int t_limit)
{
    gantt_draw(sch, t_limit);
}

/* ----------------------------------------------------------- segment list */

void viz_segment_list(const Schedule *sch)
{
    const int indent = 2;
    int i, cell = 0, per_line, col = 0;
    int avail = ui_term_width() - indent;

    ui_section("Execution segments (exact)");

    /* One pass to size the widest entry, so the columns line up. */
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        const char *label = (s->proc == IDLE_INDEX) ? IDLE_LABEL
                                                    : sch->procs[s->proc].id;
        char buf[64];
        int w = snprintf(buf, sizeof buf, "%s [%d,%d)", label, s->start,
                         s->end);
        if (w > cell) cell = w;
    }
    cell += 3;
    if (avail < cell + 1) avail = cell + 1;
    per_line = avail / cell;
    if (per_line < 1) per_line = 1;

    pad(indent);
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        const char *label = (s->proc == IDLE_INDEX) ? IDLE_LABEL
                                                    : sch->procs[s->proc].id;
        char buf[64];
        int w = snprintf(buf, sizeof buf, "%s [%d,%d)", label, s->start,
                         s->end);
        if (w < 0) w = 0;
        printf("%s%s%s", ui_c(ui_proc_color(s->proc)), buf, ui_c(C_RESET));
        col++;
        if (col >= per_line || i + 1 >= sch->seg_count) {
            printf("\n");
            if (i + 1 < sch->seg_count) pad(indent);
            col = 0;
        } else {
            /* pad only between entries, so no line ends in blanks */
            pad((cell - w > 0) ? cell - w : 1);
        }
    }
    pad(indent);
    printf("%s%d segment%s; the schedule runs from t=%d to t=%d%s\n",
           ui_c(C_DIM), sch->seg_count, (sch->seg_count == 1) ? "" : "s",
           sch->window_start, sch->window_end, ui_c(C_RESET));
}

/* -------------------------------------------------------------- summary */

void viz_summary(const Schedule *sch)
{
    const AlgorithmInfo *info = algo_info(sch->algo);

    ui_section("Performance metrics");

    kv("Algorithm", "%s (%s)", info->name, info->kind);
    if (info->needs_quantum) kv("Time quantum", "%d", sch->quantum);
    kv("Processes", "%d", sch->n);
    kv("Observation window", "t=%d to t=%d", sch->window_start,
       sch->window_end);
    kv("Total elapsed time", "%d unit(s)", sch->total_time);
    kv("CPU busy time", "%d unit(s)", sch->busy_time);
    kv("CPU idle time", "%d unit(s)", sch->idle_time);
    kv("CPU utilization", "%.2f %%", sch->cpu_utilization);
    kv("Throughput", "%.4f process(es) per unit", sch->throughput);
    kv("Context switches", "%d", sch->context_switches);
    kv("Preemptions", "%d", sch->preemptions);
    kv("Average completion time", "%.2f", sch->avg_completion);
    kv("Average turnaround time", "%.2f", sch->avg_turnaround);
    kv("Average waiting time", "%.2f", sch->avg_waiting);
    kv("Average response time", "%.2f", sch->avg_response);
}

void viz_report(const Schedule *sch)
{
    const AlgorithmInfo *info = algo_info(sch->algo);

    if (info->needs_quantum) {
        ui_title("%s -- %s (quantum = %d)", info->abbrev, info->name,
                 sch->quantum);
    } else {
        ui_title("%s -- %s (%s)", info->abbrev, info->name, info->kind);
    }
    printf("  %sdataset: %s%s\n", ui_c(C_DIM), sch->dataset_name,
           ui_c(C_RESET));
    wrap_print(info->rule, 2);

    viz_gantt(sch);
    viz_segment_list(sch);
    viz_process_table(sch);
    viz_summary(sch);
}

/* ------------------------------------------------------------- help text */

void viz_assumptions(void)
{
    ui_title("Scheduling assumptions and metric definitions");

    ui_section("Model");
    wrap_print(
        "A single CPU runs one process at a time. Time is measured in "
        "abstract integer units and every event happens on an integer "
        "boundary. There is no I/O, no blocking and no ageing: once a "
        "process arrives it stays ready until it has received its whole "
        "burst. Context switches and dispatching are assumed to cost zero "
        "time, so the switches reported in the metrics are counted but never "
        "charged to the clock -- that keeps the hand-calculated textbook "
        "answers reproducible.", 2);

    ui_section("Priority convention");
    wrap_print(
        "A LOWER priority number means a HIGHER priority. Priority 0 is the "
        "most urgent value accepted and 99 the least. Priority is ignored by "
        "FCFS, SJF, SRTF and Round Robin.", 2);

    ui_section("Deterministic tie-breaking");
    wrap_print(
        "Whenever two processes are equally good candidates, the choice is "
        "resolved in this order: (1) the algorithm's own key -- arrival time, "
        "burst, remaining time or priority; (2) the earlier arrival time; "
        "(3) the earlier position in the input. Because rule 3 always "
        "decides, the simulator never has to guess: the same dataset "
        "always produces exactly the same schedule.", 2);

    ui_section("Preemption rules");
    wrap_print(
        "SRTF and preemptive priority re-evaluate the decision at every "
        "arrival and every completion. A newly arrived process displaces the "
        "running one only when its key is STRICTLY better, so equal values "
        "never cause a gratuitous switch. Round Robin preempts on quantum "
        "expiry only; if no other process is waiting at that instant the "
        "running process simply continues, and that is neither a context "
        "switch nor a preemption.", 2);

    ui_section("Round Robin queue discipline");
    wrap_print(
        "The ready queue is strictly FIFO. At the instant a quantum expires, "
        "processes that arrived during the slice -- including any that arrive "
        "at exactly that instant -- are appended first, in arrival order, and "
        "the process whose quantum expired is appended behind them. "
        "Simultaneous arrivals enter in input order.", 2);

    ui_section("Observation window");
    wrap_print(
        "Metrics are measured over [earliest arrival, last completion]. Time "
        "before the first process exists is not charged to the CPU, so a "
        "workload whose first arrival is t=5 and whose last completion is "
        "t=20 has an elapsed time of 15 units.", 2);

    ui_section("Metric definitions");
    kv("Completion time", "end of the process's last CPU segment");
    kv("Turnaround time", "completion - arrival");
    kv("Waiting time", "turnaround - burst");
    kv("Response time", "first time on the CPU - arrival");
    kv("Idle time", "units inside the window with no process running");
    kv("CPU busy time", "window length - idle time");
    kv("CPU utilization", "100 x busy / window length");
    kv("Throughput", "processes completed / window length");
    kv("Context switches", "hand-overs between two different processes");
    kv("Preemptions", "times a process left the CPU with work remaining");
    printf("\n");
    wrap_print(
        "The first dispatch is not counted as a context switch. Adjacent "
        "slices belonging to the same process are merged before counting, "
        "which is why a Round Robin run with a single ready process reports "
        "zero switches. Every number printed by Schedulify is computed from "
        "the simulated timeline; none of it is hard-coded.", 2);
}

void viz_algorithm_rules(void)
{
    int i;

    ui_title("The six algorithms");
    for (i = 0; i < ALGO_COUNT; i++) {
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);
        printf("\n  %s%s%s -- %s%s%s (%s)\n",
               ui_c(C_TITLE), info->abbrev, ui_c(C_RESET),
               ui_c(C_HEADING), info->name, ui_c(C_RESET), info->kind);
        wrap_print(info->rule, 4);
    }
}
