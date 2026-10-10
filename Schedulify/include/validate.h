/*
 * validate.h -- All input reading and validation lives here.
 *
 * Two layers:
 *   1. Pure predicates (validate_*) that never touch stdio.  These are what
 *      the unit tests exercise and what the CSV importer reuses, so a file
 *      and a keyboard are held to exactly the same standard.
 *   2. Interactive prompts (prompt_*) that re-ask until the answer is valid,
 *      and that distinguish "the user typed q to go back" from "stdin hit
 *      end-of-file", because the first means pop one menu and the second
 *      means shut the program down cleanly.
 */
#ifndef SCHEDULIFY_VALIDATE_H
#define SCHEDULIFY_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#include "process.h"

#define INPUT_LINE_MAX 256
#define ERR_MSG_MAX    160

typedef enum {
    PROMPT_OK = 0,   /* a valid value was obtained                         */
    PROMPT_CANCEL,   /* user typed 'q' / 'b' / blank-cancel: go back       */
    PROMPT_EOF       /* stdin is exhausted: unwind and exit the program    */
} PromptResult;

/* ---- pure validation ---------------------------------------------------- */

/* A process id must be 1..PROC_ID_MAX-1 printable characters with no
 * whitespace, comma, quote or control character (so it survives CSV and
 * column layout unharmed). */
bool validate_id(const char *id, char *err, size_t errsz);
bool validate_arrival(int v, char *err, size_t errsz);
bool validate_burst(int v, char *err, size_t errsz);
bool validate_priority(int v, char *err, size_t errsz);
bool validate_quantum(int v, char *err, size_t errsz);
bool validate_process(const Process *p, char *err, size_t errsz);

/* Checks the dataset as a whole: non-empty, within MAX_PROCESSES, every
 * field valid, and no duplicate ids (compared case-insensitively). */
bool validate_dataset(const Dataset *ds, char *err, size_t errsz);

/* Strict integer parsing: the whole string must be consumed, leading and
 * trailing blanks are tolerated, overflow is rejected. */
bool parse_int(const char *s, int *out);
bool parse_int_range(const char *s, int lo, int hi, int *out);

/* Trims leading/trailing whitespace in place and returns s. */
char *trim(char *s);

/* ---- interactive input -------------------------------------------------- */

/* Reads one line from stdin into buf (newline removed).  Over-long lines are
 * reported as PROMPT_CANCEL after the remainder is discarded. */
PromptResult read_line(char *buf, size_t size);

/* True once stdin has returned EOF; lets main() exit without looping. */
bool input_at_eof(void);

PromptResult prompt_int(const char *label, int lo, int hi, int *out);

/* Same, but an empty line accepts def. */
PromptResult prompt_int_default(const char *label, int lo, int hi, int def,
                                int *out);

/* Reads a non-empty line.  'q' alone cancels. */
PromptResult prompt_text(const char *label, char *buf, size_t size);

/* Reads a validated process id that is not already present in ds
 * (pass ds == NULL to skip the duplicate check). */
PromptResult prompt_process_id(const char *label, const Dataset *ds,
                               char *buf, size_t size);

PromptResult prompt_yes_no(const char *label, bool def, bool *out);

/* Prints "Press Enter to continue" and waits. */
PromptResult prompt_pause(const char *label);

#endif /* SCHEDULIFY_VALIDATE_H */
