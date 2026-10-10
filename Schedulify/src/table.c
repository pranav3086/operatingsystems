/*
 * table.c -- Column-aligned table writer.
 */
#include "table.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) memcpy(d, s, n);
    return d;
}

void tbl_init(Table *t, int ncols, const char *const *hdrs,
              const TblAlign *aligns)
{
    int i;

    memset(t, 0, sizeof *t);
    if (ncols > TBL_MAX_COLS) ncols = TBL_MAX_COLS;
    t->ncols = ncols;
    t->cur = -1;

    for (i = 0; i < ncols; i++) {
        snprintf(t->hdr[i], sizeof t->hdr[i], "%s", hdrs[i]);
        t->align[i] = (aligns != NULL) ? aligns[i] : TBL_LEFT;
        t->width[i] = ui_display_width(t->hdr[i]);
    }
}

void tbl_free(Table *t)
{
    int r, c;
    for (r = 0; r < t->nrows; r++) {
        if (t->rows[r].cell == NULL) continue;
        for (c = 0; c < t->ncols; c++) free(t->rows[r].cell[c]);
        free(t->rows[r].cell);
    }
    free(t->rows);
    t->rows = NULL;
    t->nrows = 0;
    t->cap = 0;
}

static bool reserve(Table *t)
{
    int cap;
    TblRow *grown;

    if (t->nrows < t->cap) return true;
    cap = (t->cap > 0) ? t->cap * 2 : 16;
    grown = realloc(t->rows, (size_t)cap * sizeof *grown);
    if (grown == NULL) { t->oom = true; return false; }
    t->rows = grown;
    t->cap = cap;
    return true;
}

static void new_row(Table *t, bool separator, bool emphasis)
{
    TblRow *row;
    int c;

    if (!reserve(t)) return;
    row = &t->rows[t->nrows];
    row->separator = separator;
    row->emphasis = emphasis;
    row->cell = NULL;

    if (!separator) {
        row->cell = calloc((size_t)t->ncols, sizeof *row->cell);
        if (row->cell == NULL) { t->oom = true; return; }
        for (c = 0; c < t->ncols; c++) {
            row->cell[c] = dup_str("");
            if (row->cell[c] == NULL) t->oom = true;
        }
    }
    t->cur = t->nrows;
    t->curcol = 0;
    t->nrows++;
}

void tbl_row(Table *t)           { new_row(t, false, false); }
void tbl_row_emphasis(Table *t)  { new_row(t, false, true);  }
void tbl_separator(Table *t)     { new_row(t, true, false); t->cur = -1; }

void tbl_cellf(Table *t, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    int w;
    TblRow *row;

    if (t->cur < 0 || t->curcol >= t->ncols) return;
    row = &t->rows[t->cur];
    if (row->cell == NULL) return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    free(row->cell[t->curcol]);
    row->cell[t->curcol] = dup_str(buf);
    if (row->cell[t->curcol] == NULL) { t->oom = true; return; }

    w = ui_display_width(buf);
    if (w > t->width[t->curcol]) t->width[t->curcol] = w;
    t->curcol++;
}

static void pad(int n)
{
    int i;
    for (i = 0; i < n; i++) fputc(' ', stdout);
}

static void border(const Table *t, int indent, const char *left,
                   const char *mid, const char *right)
{
    const BoxChars *b = ui_box();
    int c;

    pad(indent);
    fputs(ui_c(C_DIM), stdout);
    fputs(left, stdout);
    for (c = 0; c < t->ncols; c++) {
        ui_repeat(b->h, t->width[c] + 2);
        fputs((c + 1 < t->ncols) ? mid : right, stdout);
    }
    printf("%s\n", ui_c(C_RESET));
}

static void print_cells(const Table *t, int indent, const TblRow *row)
{
    const BoxChars *b = ui_box();
    const char *style = row->emphasis ? ui_c(C_BOLD) : ui_c(C_VALUE);
    int c;

    pad(indent);
    printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    for (c = 0; c < t->ncols; c++) {
        const char *txt = (row->cell[c] != NULL) ? row->cell[c] : "";
        int gap = t->width[c] - ui_display_width(txt);
        if (gap < 0) gap = 0;
        if (t->align[c] == TBL_RIGHT) {
            printf(" %*s%s%s%s ", gap, "", style, txt, ui_c(C_RESET));
        } else {
            printf(" %s%s%s%*s ", style, txt, ui_c(C_RESET), gap, "");
        }
        printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    }
    fputc('\n', stdout);
}

void tbl_render(const Table *t, int indent)
{
    const BoxChars *b = ui_box();
    int c, r;

    if (t->oom) {
        ui_error("out of memory while building a table; output truncated");
    }

    border(t, indent, b->tl, b->t_down, b->tr);

    /* header */
    pad(indent);
    printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    for (c = 0; c < t->ncols; c++) {
        int gap = t->width[c] - ui_display_width(t->hdr[c]);
        if (gap < 0) gap = 0;
        if (t->align[c] == TBL_RIGHT) {
            printf(" %*s%s%s%s ", gap, "", ui_c(C_HEADING), t->hdr[c],
                   ui_c(C_RESET));
        } else {
            printf(" %s%s%s%*s ", ui_c(C_HEADING), t->hdr[c], ui_c(C_RESET),
                   gap, "");
        }
        printf("%s%s%s", ui_c(C_DIM), b->v, ui_c(C_RESET));
    }
    fputc('\n', stdout);

    border(t, indent, b->t_right, b->cross, b->t_left);

    for (r = 0; r < t->nrows; r++) {
        if (t->rows[r].separator) {
            border(t, indent, b->t_right, b->cross, b->t_left);
        } else {
            print_cells(t, indent, &t->rows[r]);
        }
    }

    border(t, indent, b->bl, b->t_up, b->br);
}
