/*
 * table.h -- Minimal column-aligned table writer used by visualize.c and
 * compare.c.  Cells are collected first so that column widths can be
 * computed before anything is printed; the glyphs come from ui.c, so the
 * same table renders in Unicode or in plain ASCII.
 */
#ifndef SCHEDULIFY_TABLE_H
#define SCHEDULIFY_TABLE_H

#include <stdbool.h>

#define TBL_MAX_COLS 14

typedef enum { TBL_LEFT = 0, TBL_RIGHT } TblAlign;

typedef struct {
    char **cell;      /* ncols strings, or NULL for a separator row */
    bool   separator;
    bool   emphasis;  /* render the row in bold (used for the averages row) */
} TblRow;

typedef struct {
    int       ncols;
    char      hdr[TBL_MAX_COLS][32];
    TblAlign  align[TBL_MAX_COLS];
    int       width[TBL_MAX_COLS];
    TblRow   *rows;
    int       nrows;
    int       cap;
    int       cur;    /* row currently being filled, -1 when none */
    int       curcol;
    bool      oom;
} Table;

/* hdrs and aligns must both have ncols entries. */
void tbl_init(Table *t, int ncols, const char *const *hdrs,
              const TblAlign *aligns);
void tbl_free(Table *t);

void tbl_row(Table *t);            /* start a new ordinary row            */
void tbl_row_emphasis(Table *t);   /* start a new bold row                */
void tbl_separator(Table *t);      /* horizontal rule between rows        */

/* Appends one cell to the current row. */
void tbl_cellf(Table *t, const char *fmt, ...);

/* Prints the whole table, indented by `indent` spaces. */
void tbl_render(const Table *t, int indent);

#endif /* SCHEDULIFY_TABLE_H */
