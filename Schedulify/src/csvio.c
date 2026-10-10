/*
 * csvio.c -- CSV import and export.
 */
#include "csvio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "validate.h"

#define CSV_LINE_MAX 512

/* Splits `line` in place on commas.  Returns the number of fields found,
 * writing pointers to the trimmed fields into out[0..max-1]. */
static int split_fields(char *line, char **out, int max)
{
    int n = 0;
    char *p = line;

    while (n < max) {
        char *comma = strchr(p, ',');
        if (comma != NULL) *comma = '\0';
        out[n++] = trim(p);
        if (comma == NULL) break;
        p = comma + 1;
    }
    return n;
}

static bool ci_prefix(const char *s, const char *word)
{
    size_t i;
    for (i = 0; word[i] != '\0'; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)word[i]))
            return false;
    }
    return true;
}

static bool is_header(char **f, int nf)
{
    if (nf < 3) return false;
    return ci_prefix(f[0], "id") || ci_prefix(f[0], "process")
        || ci_prefix(f[0], "pid") || ci_prefix(f[0], "name");
}

bool csv_import_dataset(const char *path, Dataset *ds, char *err, size_t errsz)
{
    FILE *fp;
    char line[CSV_LINE_MAX];
    int lineno = 0;
    bool first_data_line = true;
    char verr[ERR_MSG_MAX];

    dataset_init(ds, "imported");

    fp = fopen(path, "r");
    if (fp == NULL) {
        snprintf(err, errsz, "cannot open \"%s\" for reading", path);
        return false;
    }

    while (fgets(line, sizeof line, fp) != NULL) {
        char *fields[8];
        int nf;
        Process p;
        char *text = line;
        size_t len;

        lineno++;

        /* Strip a UTF-8 BOM on the very first line. */
        if (lineno == 1 && (unsigned char)text[0] == 0xEF
            && (unsigned char)text[1] == 0xBB
            && (unsigned char)text[2] == 0xBF) {
            text += 3;
        }

        len = strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            text[--len] = '\0';

        trim(text);
        if (text[0] == '\0' || text[0] == '#') continue;

        nf = split_fields(text, fields, 8);

        if (first_data_line && is_header(fields, nf)) {
            first_data_line = false;
            continue;
        }
        first_data_line = false;

        if (nf < 3) {
            snprintf(err, errsz,
                     "line %d: expected at least 3 comma-separated fields "
                     "(id,arrival,burst[,priority]) but found %d",
                     lineno, nf);
            goto fail;
        }

        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "%s", fields[0]);
        if (strlen(fields[0]) >= PROC_ID_MAX) {
            snprintf(err, errsz,
                     "line %d: process id \"%s\" is longer than %d characters",
                     lineno, fields[0], PROC_ID_MAX - 1);
            goto fail;
        }
        if (!parse_int(fields[1], &p.arrival)) {
            snprintf(err, errsz, "line %d: arrival \"%s\" is not an integer",
                     lineno, fields[1]);
            goto fail;
        }
        if (!parse_int(fields[2], &p.burst)) {
            snprintf(err, errsz, "line %d: burst \"%s\" is not an integer",
                     lineno, fields[2]);
            goto fail;
        }
        if (nf >= 4 && fields[3][0] != '\0') {
            if (!parse_int(fields[3], &p.priority)) {
                snprintf(err, errsz,
                         "line %d: priority \"%s\" is not an integer",
                         lineno, fields[3]);
                goto fail;
            }
        } else {
            p.priority = MIN_PRIORITY;
        }

        if (!validate_process(&p, verr, sizeof verr)) {
            snprintf(err, errsz, "line %d: %s", lineno, verr);
            goto fail;
        }
        if (dataset_find_id(ds, p.id) >= 0) {
            snprintf(err, errsz, "line %d: duplicate process id \"%s\"",
                     lineno, p.id);
            goto fail;
        }
        if (!dataset_add(ds, &p)) {
            snprintf(err, errsz,
                     "line %d: cannot hold more than %d processes",
                     lineno, MAX_PROCESSES);
            goto fail;
        }
    }

    if (ferror(fp)) {
        snprintf(err, errsz, "error while reading \"%s\"", path);
        goto fail;
    }
    fclose(fp);

    if (ds->count == 0) {
        snprintf(err, errsz, "\"%s\" contains no process rows", path);
        dataset_free(ds);
        dataset_init(ds, "imported");
        return false;
    }

    /* Name the dataset after the file, without directories or extension. */
    {
        const char *base = path;
        const char *q;
        char name[DATASET_NAME_MAX];
        size_t i = 0;

        for (q = path; *q != '\0'; q++) {
            if (*q == '/' || *q == '\\') base = q + 1;
        }
        for (; base[i] != '\0' && i + 1 < sizeof name; i++) name[i] = base[i];
        name[i] = '\0';
        for (i = strlen(name); i > 0; i--) {
            if (name[i - 1] == '.') { name[i - 1] = '\0'; break; }
        }
        dataset_set_name(ds, (name[0] != '\0') ? name : "imported");
    }

    err[0] = '\0';
    return true;

fail:
    fclose(fp);
    dataset_free(ds);
    dataset_init(ds, "imported");
    return false;
}

/* --------------------------------------------------------------- export */

static FILE *open_write(const char *path, char *err, size_t errsz)
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL)
        snprintf(err, errsz, "cannot open \"%s\" for writing", path);
    return fp;
}

static bool close_ok(FILE *fp, const char *path, char *err, size_t errsz)
{
    bool bad = ferror(fp) != 0;
    if (fclose(fp) != 0) bad = true;
    if (bad) {
        snprintf(err, errsz, "error while writing \"%s\"", path);
        return false;
    }
    err[0] = '\0';
    return true;
}

bool csv_export_dataset(const char *path, const Dataset *ds, char *err,
                        size_t errsz)
{
    FILE *fp = open_write(path, err, errsz);
    int i;

    if (fp == NULL) return false;
    fprintf(fp, "# Schedulify workload \"%s\"\n", ds->name);
    fprintf(fp, "# lower priority number == higher priority\n");
    fprintf(fp, "id,arrival,burst,priority\n");
    for (i = 0; i < ds->count; i++) {
        fprintf(fp, "%s,%d,%d,%d\n", ds->items[i].id, ds->items[i].arrival,
                ds->items[i].burst, ds->items[i].priority);
    }
    return close_ok(fp, path, err, errsz);
}

bool csv_export_schedule(const char *path, const Schedule *sch, char *err,
                         size_t errsz)
{
    const AlgorithmInfo *info = algo_info(sch->algo);
    FILE *fp = open_write(path, err, errsz);
    int i;

    if (fp == NULL) return false;

    fprintf(fp, "# Schedulify result -- %s (%s)", info->name, info->kind);
    if (info->needs_quantum) fprintf(fp, ", quantum=%d", sch->quantum);
    fprintf(fp, "\n# dataset: %s\n", sch->dataset_name);

    fprintf(fp, "section,id,arrival,burst,priority,start,completion,"
                "turnaround,waiting,response\n");
    for (i = 0; i < sch->n; i++) {
        const Process *p = &sch->procs[i];
        const ProcStats *s = &sch->stats[i];
        fprintf(fp, "process,%s,%d,%d,%d,%d,%d,%d,%d,%d\n",
                p->id, p->arrival, p->burst, p->priority,
                s->first_start, s->completion, s->turnaround, s->waiting,
                s->response);
    }

    fprintf(fp, "\nsection,metric,value\n");
    fprintf(fp, "summary,algorithm,%s\n", info->abbrev);
    fprintf(fp, "summary,algorithm_name,%s %s\n", info->name, info->kind);
    if (info->needs_quantum)
        fprintf(fp, "summary,time_quantum,%d\n", sch->quantum);
    fprintf(fp, "summary,processes,%d\n", sch->n);
    fprintf(fp, "summary,window_start,%d\n", sch->window_start);
    fprintf(fp, "summary,window_end,%d\n", sch->window_end);
    fprintf(fp, "summary,total_time,%d\n", sch->total_time);
    fprintf(fp, "summary,busy_time,%d\n", sch->busy_time);
    fprintf(fp, "summary,idle_time,%d\n", sch->idle_time);
    fprintf(fp, "summary,cpu_utilization_percent,%.4f\n",
            sch->cpu_utilization);
    fprintf(fp, "summary,throughput_per_unit,%.6f\n", sch->throughput);
    fprintf(fp, "summary,context_switches,%d\n", sch->context_switches);
    fprintf(fp, "summary,preemptions,%d\n", sch->preemptions);
    fprintf(fp, "summary,avg_completion,%.4f\n", sch->avg_completion);
    fprintf(fp, "summary,avg_turnaround,%.4f\n", sch->avg_turnaround);
    fprintf(fp, "summary,avg_waiting,%.4f\n", sch->avg_waiting);
    fprintf(fp, "summary,avg_response,%.4f\n", sch->avg_response);

    fprintf(fp, "\nsection,index,process,start,end,duration\n");
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        fprintf(fp, "segment,%d,%s,%d,%d,%d\n", i,
                (s->proc == IDLE_INDEX) ? "IDLE" : sch->procs[s->proc].id,
                s->start, s->end, s->end - s->start);
    }

    return close_ok(fp, path, err, errsz);
}

bool csv_export_timeline(const char *path, const Schedule *sch, char *err,
                         size_t errsz)
{
    FILE *fp = open_write(path, err, errsz);
    int i;

    if (fp == NULL) return false;
    fprintf(fp, "# Schedulify timeline -- %s on \"%s\"\n",
            algo_abbrev(sch->algo), sch->dataset_name);
    fprintf(fp, "index,process,start,end,duration\n");
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        fprintf(fp, "%d,%s,%d,%d,%d\n", i,
                (s->proc == IDLE_INDEX) ? "IDLE" : sch->procs[s->proc].id,
                s->start, s->end, s->end - s->start);
    }
    return close_ok(fp, path, err, errsz);
}

bool csv_export_comparison(const char *path, const Comparison *cmp,
                           char *err, size_t errsz)
{
    FILE *fp = open_write(path, err, errsz);
    int i;

    if (fp == NULL) return false;

    fprintf(fp, "# Schedulify comparison -- dataset \"%s\", %d process(es), "
                "round robin quantum %d\n",
            cmp->dataset_name, cmp->n, cmp->quantum);
    fprintf(fp, "algorithm,kind,quantum,avg_turnaround,avg_waiting,"
                "avg_response,avg_completion,cpu_utilization_percent,"
                "throughput_per_unit,context_switches,preemptions,"
                "total_time,busy_time,idle_time\n");

    for (i = 0; i < ALGO_COUNT; i++) {
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);
        const Schedule *s = &cmp->sch[i];

        if (!cmp->ok[i]) {
            fprintf(fp, "%s,%s,,FAILED,,,,,,,,,,\n", info->abbrev,
                    info->kind);
            continue;
        }
        fprintf(fp, "%s,%s,", info->abbrev, info->kind);
        if (info->needs_quantum) fprintf(fp, "%d", cmp->quantum);
        fprintf(fp, ",%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%d,%d,%d,%d,%d\n",
                s->avg_turnaround, s->avg_waiting, s->avg_response,
                s->avg_completion, s->cpu_utilization, s->throughput,
                s->context_switches, s->preemptions, s->total_time,
                s->busy_time, s->idle_time);
    }

    return close_ok(fp, path, err, errsz);
}
