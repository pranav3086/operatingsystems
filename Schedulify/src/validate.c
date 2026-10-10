/*
 * validate.c -- Input reading and validation.
 */
#include "validate.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

static bool g_eof = false;

bool input_at_eof(void)
{
    return g_eof;
}

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

char *trim(char *s)
{
    char *start = s;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

/* ----------------------------------------------------------- pure checks */

bool validate_id(const char *id, char *err, size_t errsz)
{
    size_t len, i;

    if (id == NULL || id[0] == '\0') {
        set_err(err, errsz, "process id must not be empty");
        return false;
    }
    len = strlen(id);
    if (len >= PROC_ID_MAX) {
        set_err(err, errsz, "process id \"%s\" is longer than %d characters",
                id, PROC_ID_MAX - 1);
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isgraph(c) || c == ',' || c == '"' || c == '\'' || c == ';') {
            set_err(err, errsz,
                    "process id \"%s\" may not contain spaces, commas, "
                    "semicolons or quotes", id);
            return false;
        }
    }
    return true;
}

bool validate_arrival(int v, char *err, size_t errsz)
{
    if (v < MIN_ARRIVAL || v > MAX_ARRIVAL) {
        set_err(err, errsz, "arrival time %d is outside %d..%d", v,
                MIN_ARRIVAL, MAX_ARRIVAL);
        return false;
    }
    return true;
}

bool validate_burst(int v, char *err, size_t errsz)
{
    if (v < MIN_BURST || v > MAX_BURST) {
        set_err(err, errsz,
                "burst time %d is outside %d..%d (a process must ask for at "
                "least one time unit of CPU)", v, MIN_BURST, MAX_BURST);
        return false;
    }
    return true;
}

bool validate_priority(int v, char *err, size_t errsz)
{
    if (v < MIN_PRIORITY || v > MAX_PRIORITY) {
        set_err(err, errsz, "priority %d is outside %d..%d", v,
                MIN_PRIORITY, MAX_PRIORITY);
        return false;
    }
    return true;
}

bool validate_quantum(int v, char *err, size_t errsz)
{
    if (v < MIN_QUANTUM || v > MAX_QUANTUM) {
        set_err(err, errsz, "time quantum %d is outside %d..%d", v,
                MIN_QUANTUM, MAX_QUANTUM);
        return false;
    }
    return true;
}

bool validate_process(const Process *p, char *err, size_t errsz)
{
    return validate_id(p->id, err, errsz)
        && validate_arrival(p->arrival, err, errsz)
        && validate_burst(p->burst, err, errsz)
        && validate_priority(p->priority, err, errsz);
}

bool validate_dataset(const Dataset *ds, char *err, size_t errsz)
{
    int i, j;

    if (ds == NULL || ds->count <= 0) {
        set_err(err, errsz, "the dataset is empty -- add at least one process");
        return false;
    }
    if (ds->count > MAX_PROCESSES) {
        set_err(err, errsz, "the dataset holds %d processes (maximum is %d)",
                ds->count, MAX_PROCESSES);
        return false;
    }
    for (i = 0; i < ds->count; i++) {
        char sub[ERR_MSG_MAX];
        if (!validate_process(&ds->items[i], sub, sizeof sub)) {
            set_err(err, errsz, "row %d: %s", i + 1, sub);
            return false;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(ds->items[i].id, ds->items[j].id) == 0) {
                set_err(err, errsz,
                        "duplicate process id \"%s\" in rows %d and %d",
                        ds->items[i].id, j + 1, i + 1);
                return false;
            }
        }
    }
    /* Guard against an overflowing timeline: the makespan can never exceed
     * the last arrival plus the sum of all bursts. */
    {
        long total = 0;
        int max_arrival = 0;
        for (i = 0; i < ds->count; i++) {
            total += ds->items[i].burst;
            if (ds->items[i].arrival > max_arrival)
                max_arrival = ds->items[i].arrival;
        }
        if (total + max_arrival > INT_MAX / 2) {
            set_err(err, errsz, "the total workload is too large to simulate");
            return false;
        }
    }
    return true;
}

bool parse_int(const char *s, int *out)
{
    char buf[INPUT_LINE_MAX];
    char *endp;
    long v;

    if (s == NULL) return false;
    if (strlen(s) >= sizeof buf) return false;
    snprintf(buf, sizeof buf, "%s", s);
    trim(buf);
    if (buf[0] == '\0') return false;

    errno = 0;
    v = strtol(buf, &endp, 10);
    if (endp == buf || *endp != '\0') return false;
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;

    *out = (int)v;
    return true;
}

bool parse_int_range(const char *s, int lo, int hi, int *out)
{
    int v;
    if (!parse_int(s, &v)) return false;
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* --------------------------------------------------------------- stdin IO */


/* When input is piped or redirected the terminal never echoes what was
 * typed, which runs the prompt and the next line of output together.  Echo
 * it ourselves so a scripted transcript is as readable as a live session. */
static void echo_if_not_tty(const char *line)
{
    if (ui_stdin_is_tty()) return;
    printf("%s\n", (line != NULL) ? line : "");
    fflush(stdout);
}

PromptResult read_line(char *buf, size_t size)
{
    size_t len;

    if (size == 0) return PROMPT_CANCEL;
    if (g_eof) return PROMPT_EOF;

    if (fgets(buf, (int)size, stdin) == NULL) {
        g_eof = true;
        buf[0] = '\0';
        return PROMPT_EOF;
    }

    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        if (len >= 2 && buf[len - 2] == '\r') buf[len - 2] = '\0';
        echo_if_not_tty(buf);
        return PROMPT_OK;
    }
    /* No newline: either the line is longer than the buffer or EOF was hit
     * mid-line.  Drain the remainder so the next read starts clean. */
    {
        int c, extra = 0;
        while ((c = fgetc(stdin)) != EOF && c != '\n') extra++;
        if (c == EOF && extra == 0 && len == 0) {
            g_eof = true;
            return PROMPT_EOF;
        }
        if (extra > 0) {
            ui_error("input was longer than %d characters and was discarded",
                     (int)size - 1);
            return PROMPT_CANCEL;
        }
    }
    echo_if_not_tty(buf);
    return PROMPT_OK;
}

static bool is_cancel_word(const char *s)
{
    return strcmp(s, "q") == 0 || strcmp(s, "Q") == 0
        || strcmp(s, "b") == 0 || strcmp(s, "B") == 0
        || strcmp(s, "quit") == 0 || strcmp(s, "back") == 0
        || strcmp(s, "cancel") == 0;
}

PromptResult prompt_int(const char *label, int lo, int hi, int *out)
{
    char buf[INPUT_LINE_MAX];

    for (;;) {
        PromptResult r;
        ui_prompt("%s [%d..%d] (q to go back): ", label, lo, hi);
        r = read_line(buf, sizeof buf);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL) continue;

        trim(buf);
        if (is_cancel_word(buf)) return PROMPT_CANCEL;
        if (buf[0] == '\0') {
            ui_error("please type a number between %d and %d", lo, hi);
            continue;
        }
        if (!parse_int_range(buf, lo, hi, out)) {
            ui_error("\"%s\" is not a whole number between %d and %d",
                     buf, lo, hi);
            continue;
        }
        return PROMPT_OK;
    }
}

PromptResult prompt_int_default(const char *label, int lo, int hi, int def,
                                int *out)
{
    char buf[INPUT_LINE_MAX];

    for (;;) {
        PromptResult r;
        ui_prompt("%s [%d..%d, Enter for %d] (q to go back): ",
                  label, lo, hi, def);
        r = read_line(buf, sizeof buf);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL) continue;

        trim(buf);
        if (is_cancel_word(buf)) return PROMPT_CANCEL;
        if (buf[0] == '\0') {
            *out = def;
            return PROMPT_OK;
        }
        if (!parse_int_range(buf, lo, hi, out)) {
            ui_error("\"%s\" is not a whole number between %d and %d",
                     buf, lo, hi);
            continue;
        }
        return PROMPT_OK;
    }
}

PromptResult prompt_text(const char *label, char *buf, size_t size)
{
    for (;;) {
        PromptResult r;
        ui_prompt("%s (q to go back): ", label);
        r = read_line(buf, size);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL) continue;

        trim(buf);
        if (is_cancel_word(buf)) return PROMPT_CANCEL;
        if (buf[0] == '\0') {
            ui_error("the value must not be empty");
            continue;
        }
        return PROMPT_OK;
    }
}

PromptResult prompt_process_id(const char *label, const Dataset *ds,
                               char *buf, size_t size)
{
    for (;;) {
        char err[ERR_MSG_MAX];
        PromptResult r = prompt_text(label, buf, size);
        if (r != PROMPT_OK) return r;

        if (!validate_id(buf, err, sizeof err)) {
            ui_error("%s", err);
            continue;
        }
        if (ds != NULL && dataset_find_id(ds, buf) >= 0) {
            ui_error("process id \"%s\" is already used in this dataset", buf);
            continue;
        }
        return PROMPT_OK;
    }
}

PromptResult prompt_yes_no(const char *label, bool def, bool *out)
{
    char buf[INPUT_LINE_MAX];

    for (;;) {
        PromptResult r;
        ui_prompt("%s [y/n, Enter for %s]: ", label, def ? "yes" : "no");
        r = read_line(buf, sizeof buf);
        if (r == PROMPT_EOF) return PROMPT_EOF;
        if (r == PROMPT_CANCEL) continue;

        trim(buf);
        if (buf[0] == '\0') {
            *out = def;
            return PROMPT_OK;
        }
        if (buf[0] == 'y' || buf[0] == 'Y') { *out = true;  return PROMPT_OK; }
        if (buf[0] == 'n' || buf[0] == 'N') { *out = false; return PROMPT_OK; }
        ui_error("please answer y or n");
    }
}

PromptResult prompt_pause(const char *label)
{
    char buf[INPUT_LINE_MAX];
    PromptResult r;

    ui_prompt("%s", (label != NULL) ? label : "Press Enter to continue... ");
    r = read_line(buf, sizeof buf);
    return (r == PROMPT_EOF) ? PROMPT_EOF : PROMPT_OK;
}
