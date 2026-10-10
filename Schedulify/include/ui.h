/*
 * ui.h -- Terminal presentation layer: colors, box glyphs, headings.
 *
 * Every escape sequence and every non-ASCII glyph in Schedulify is funnelled
 * through this module.  Two switches control it:
 *
 *   colors   ANSI SGR sequences.  Enabled only when stdout is a terminal,
 *            NO_COLOR is unset, and (on Windows) virtual-terminal processing
 *            could actually be turned on.
 *   unicode  box-drawing glyphs.  When off, every table and Gantt chart is
 *            drawn with +, - and | instead, which is the plain-text fallback.
 *
 * Both can be forced on or off from the Settings menu or with the
 * --color/--no-color and --unicode/--ascii command-line flags, so the output
 * is reproducible when it is piped to a file or a pager.
 */
#ifndef SCHEDULIFY_UI_H
#define SCHEDULIFY_UI_H

#include <stdbool.h>

typedef enum {
    C_RESET = 0,
    C_BOLD,
    C_DIM,
    C_TITLE,     /* bright cyan                                            */
    C_HEADING,   /* bright white on blue-ish                               */
    C_LABEL,     /* yellow                                                 */
    C_VALUE,     /* bright white                                           */
    C_OK,        /* green                                                  */
    C_WARN,      /* yellow                                                 */
    C_ERR,       /* red                                                    */
    C_IDLE,      /* dim grey, used for idle Gantt cells                    */
    C_ACCENT,    /* magenta                                                */
    C_COUNT
} UiColor;

/* Box-drawing glyph set; one of the two tables in ui.c is selected
 * according to the unicode switch. */
typedef struct {
    const char *h;      /* horizontal line                                */
    const char *v;      /* vertical line                                  */
    const char *tl, *tr, *bl, *br;        /* corners                      */
    const char *t_down, *t_up;            /* T pointing down / up         */
    const char *t_right, *t_left, *cross; /* side tees and crossing       */
    const char *arrow;                    /* "->"                         */
    const char *bullet;
} BoxChars;

/* Detects capabilities, enables VT processing and UTF-8 output on Windows. */
void ui_init(void);

void ui_set_color_enabled(bool on);
void ui_set_unicode_enabled(bool on);
bool ui_color_enabled(void);
bool ui_unicode_enabled(void);

/* True when stdin is an interactive terminal.  When it is not -- input is
 * coming from a file or a pipe -- read_line() echoes the line it consumed,
 * so a scripted transcript reads exactly like an interactive session. */
bool ui_stdin_is_tty(void);

/* Width used for layout.  Detected from the terminal, overridable. */
int  ui_term_width(void);
void ui_set_term_width(int w);

const BoxChars *ui_box(void);

/* Returns the escape sequence for a color, or "" when colors are off, so
 * it is always safe to interpolate into printf. */
const char *ui_c(UiColor c);

/* Width in display columns of a UTF-8 string (counts code points, which is
 * exact for every glyph Schedulify prints). */
int ui_display_width(const char *s);

void ui_clear_screen(void);

/* Prints glyph n times. */
void ui_repeat(const char *glyph, int n);

/* A full-width double-ruled banner with the program name. */
void ui_banner(void);

/* Boxed heading, e.g.  +--------------------+
 *                      | Round Robin (q=2)  |
 *                      +--------------------+  */
void ui_title(const char *fmt, ...);

/* Underlined sub-heading. */
void ui_section(const char *fmt, ...);

/* Full-width horizontal rule. */
void ui_rule(void);

/* Prints text word-wrapped to the output width, every line indented by
 * `indent` spaces. */
void ui_wrap(const char *text, int indent);

/* Same, rendered dim -- used for the explanatory notes under a table. */
void ui_wrap_dim(const char *text, int indent);

/* Wraps `text` with continuation lines at column `indent`, assuming the
 * cursor already sits at column `start_col`.  Prints no leading indent and
 * no trailing newline, so a caller can close a color span and add its own.
 * This is what lets a dot-leader label or a heading keep its value flowing
 * in the same paragraph. */
void ui_wrap_at(const char *text, int indent, int start_col);

void ui_info(const char *fmt, ...);
void ui_success(const char *fmt, ...);
void ui_warn(const char *fmt, ...);
void ui_error(const char *fmt, ...);
void ui_note(const char *fmt, ...);

/* Writes the prompt and flushes stdout (no newline). */
void ui_prompt(const char *fmt, ...);

/* "  [3] Compare all algorithms" */
void ui_menu_item(const char *key, const char *text);
void ui_menu_item_dim(const char *key, const char *text);

/* A stable color for process index i, used to tint Gantt cells. */
UiColor ui_proc_color(int index);

#endif /* SCHEDULIFY_UI_H */
