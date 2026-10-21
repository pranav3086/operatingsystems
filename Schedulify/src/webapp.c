/*
 * webapp.c -- Dashboard routes: JSON in, C scheduler, JSON out.
 */
#include "webapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "compare.h"
#include "datasets.h"
#include "json.h"
#include "metrics.h"
#include "predict.h"
#include "process.h"
#include "validate.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define JSON_CTYPE "application/json; charset=utf-8"

/* ------------------------------------------------------ locating the web */

static bool file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return false;
    fclose(fp);
    return true;
}

static bool root_has_page(const char *root)
{
    char probe[WEB_ROOT_MAX + 32];
    if (root == NULL || root[0] == '\0') return false;
    snprintf(probe, sizeof probe, "%s/index.html", root);
    return file_exists(probe);
}

/* Directory holding the running executable, or "" when it cannot be found. */
static void exe_dir(const char *argv0, char *out, size_t outsz)
{
    out[0] = '\0';

#if defined(_WIN32)
    {
        char buf[WEB_ROOT_MAX];
        DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
        if (n > 0 && n < sizeof buf) {
            snprintf(out, outsz, "%s", buf);
        }
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", out, outsz - 1);
        if (n > 0) out[n] = '\0';
        else       out[0] = '\0';
    }
#endif

    if (out[0] == '\0' && argv0 != NULL) snprintf(out, outsz, "%s", argv0);

    /* strip the file name */
    {
        size_t i = strlen(out);
        while (i > 0 && out[i - 1] != '/' && out[i - 1] != '\\') i--;
        if (i > 1) out[i - 1] = '\0';
        else       out[0] = '\0';
    }
}

bool webapp_init(WebApp *app, const char *explicit_root, const char *argv0,
                 char *err, size_t errsz)
{
    char exe[WEB_ROOT_MAX];
    char cand[WEB_ROOT_MAX];

    memset(app, 0, sizeof *app);

    if (explicit_root != NULL && explicit_root[0] != '\0') {
        if (root_has_page(explicit_root)) {
            snprintf(app->web_root, sizeof app->web_root, "%s",
                     explicit_root);
            app->have_root = true;
            return true;
        }
        snprintf(err, errsz, "no index.html in \"%s\"", explicit_root);
        return false;
    }

    if (root_has_page("web")) {
        snprintf(app->web_root, sizeof app->web_root, "web");
        app->have_root = true;
        return true;
    }
    if (root_has_page("../web")) {
        snprintf(app->web_root, sizeof app->web_root, "../web");
        app->have_root = true;
        return true;
    }

    exe_dir(argv0, exe, sizeof exe);
    if (exe[0] != '\0') {
        snprintf(cand, sizeof cand, "%s/web", exe);
        if (root_has_page(cand)) {
            snprintf(app->web_root, sizeof app->web_root, "%s", cand);
            app->have_root = true;
            return true;
        }
        snprintf(cand, sizeof cand, "%s/../web", exe);
        if (root_has_page(cand)) {
            snprintf(app->web_root, sizeof app->web_root, "%s", cand);
            app->have_root = true;
            return true;
        }
    }

    snprintf(err, errsz,
             "could not find the dashboard's web/ directory (looked in "
             "./web, ../web and next to the executable) -- run from the "
             "Schedulify directory or pass --web-root DIR");
    return false;
}

/* ----------------------------------------------------- static file route */

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return JSON_CTYPE;
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    return "application/octet-stream";
}

/* Only these may be served, and only from the web root. */
static bool extension_allowed(const char *path)
{
    static const char *const OK[] = { ".html", ".css", ".js", ".json",
                                      ".svg", ".png", ".ico", ".woff2" };
    const char *dot = strrchr(path, '.');
    size_t i;
    if (dot == NULL) return false;
    for (i = 0; i < sizeof OK / sizeof OK[0]; i++) {
        if (strcmp(dot, OK[i]) == 0) return true;
    }
    return false;
}

/* Rejects anything that could climb out of the web root. */
static bool path_is_safe(const char *rel)
{
    const char *p;
    if (rel == NULL || rel[0] == '\0') return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (strstr(rel, "..") != NULL) return false;
    for (p = rel; *p != '\0'; p++) {
        if (*p == ':' || *p == '\\') return false;
        if ((unsigned char)*p < 0x20) return false;
    }
    return true;
}

static void serve_static(WebApp *app, const char *rel, HttpResponse *res)
{
    char full[WEB_ROOT_MAX * 2];
    FILE *fp;
    long size;
    char *buf;
    size_t got;

    if (!path_is_safe(rel) || !extension_allowed(rel)) {
        http_respond_json_error(res, 404, "not found");
        return;
    }

    snprintf(full, sizeof full, "%s/%s", app->web_root, rel);
    fp = fopen(full, "rb");
    if (fp == NULL) {
        http_respond_json_error(res, 404, "not found");
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp);
        http_respond_json_error(res, 500, "cannot read file"); return; }
    size = ftell(fp);
    if (size < 0 || size > 8L * 1024L * 1024L) {
        fclose(fp);
        http_respond_json_error(res, 500, "file too large");
        return;
    }
    rewind(fp);

    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        http_respond_json_error(res, 500, "out of memory");
        return;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';

    http_respond_owned(res, 200, content_type_for(rel), buf, got);
}

/* ------------------------------------------------- Schedule -> JSON ---- */

static void write_algorithm(Json *j, AlgorithmId id, int quantum)
{
    const AlgorithmInfo *info = algo_info(id);
    json_obj_begin(j);
    json_kv_str(j, "key", info->abbrev);
    json_kv_str(j, "name", info->name);
    json_kv_str(j, "kind", info->kind);
    json_kv_bool(j, "preemptive", info->preemptive);
    json_kv_bool(j, "needsQuantum", info->needs_quantum);
    json_kv_str(j, "summary", info->summary);
    json_kv_str(j, "rule", info->rule);
    if (info->needs_quantum) json_kv_int(j, "quantum", quantum);
    json_obj_end(j);
}

/* predicted_from: index of the first predicted process, or -1 when the
 * workload is entirely user-entered. The merge keeps originals first. */
static void write_schedule(Json *j, const Schedule *sch, int predicted_from)
{
    int i;

    json_obj_begin(j);

    json_key(j, "algorithm");
    write_algorithm(j, sch->algo, sch->quantum);

    json_kv_str(j, "dataset", sch->dataset_name);

    json_key(j, "window");
    json_obj_begin(j);
    json_kv_int(j, "start", sch->window_start);
    json_kv_int(j, "end", sch->window_end);
    json_obj_end(j);

    json_key(j, "processes");
    json_arr_begin(j);
    for (i = 0; i < sch->n; i++) {
        const Process *p = &sch->procs[i];
        const ProcStats *s = &sch->stats[i];
        json_obj_begin(j);
        json_kv_int(j, "index", i);
        json_kv_str(j, "id", p->id);
        json_kv_int(j, "arrival", p->arrival);
        json_kv_int(j, "burst", p->burst);
        json_kv_int(j, "priority", p->priority);
        json_kv_int(j, "start", s->first_start);
        json_kv_int(j, "completion", s->completion);
        json_kv_int(j, "turnaround", s->turnaround);
        json_kv_int(j, "waiting", s->waiting);
        json_kv_int(j, "response", s->response);
        json_kv_bool(j, "predicted",
                     predicted_from >= 0 && i >= predicted_from);
        json_obj_end(j);
    }
    json_arr_end(j);

    json_key(j, "segments");
    json_arr_begin(j);
    for (i = 0; i < sch->seg_count; i++) {
        const Segment *s = &sch->segments[i];
        bool idle = (s->proc == IDLE_INDEX);
        json_obj_begin(j);
        json_kv_int(j, "start", s->start);
        json_kv_int(j, "end", s->end);
        json_kv_bool(j, "idle", idle);
        json_kv_int(j, "process", s->proc);
        json_kv_str(j, "id", idle ? "IDLE" : sch->procs[s->proc].id);
        json_obj_end(j);
    }
    json_arr_end(j);

    json_key(j, "metrics");
    json_obj_begin(j);
    json_kv_int(j, "processes", sch->n);
    json_kv_int(j, "totalTime", sch->total_time);
    json_kv_int(j, "busyTime", sch->busy_time);
    json_kv_int(j, "idleTime", sch->idle_time);
    json_kv_double(j, "cpuUtilization", sch->cpu_utilization, 4);
    json_kv_double(j, "throughput", sch->throughput, 6);
    json_kv_int(j, "contextSwitches", sch->context_switches);
    json_kv_int(j, "preemptions", sch->preemptions);
    json_kv_double(j, "avgCompletion", sch->avg_completion, 4);
    json_kv_double(j, "avgTurnaround", sch->avg_turnaround, 4);
    json_kv_double(j, "avgWaiting", sch->avg_waiting, 4);
    json_kv_double(j, "avgResponse", sch->avg_response, 4);
    json_obj_end(j);

    json_key(j, "trace");
    json_arr_begin(j);
    for (i = 0; i < sch->trace.count; i++) {
        json_obj_begin(j);
        json_kv_int(j, "time", sch->trace.items[i].time);
        json_kv_str(j, "text", sch->trace.items[i].text);
        json_obj_end(j);
    }
    json_arr_end(j);

    json_obj_end(j);
}

/* ------------------------------------------------- JSON -> workload ---- */

/* Reads an array of process objects into ds (already initialised), through
 * the same validate.c predicates the keyboard and the CSV importer use.
 * `what` names the list in messages ("process", "predicted process"). */
static bool read_processes(const JsonValue *arr, Dataset *ds,
                           const char *what, char *err, size_t errsz)
{
    int n = json_len(arr), i;

    if (n > MAX_PROCESSES) {
        snprintf(err, errsz, "at most %d processes are supported",
                 MAX_PROCESSES);
        return false;
    }

    for (i = 0; i < n; i++) {
        const JsonValue *o = json_at(arr, i);
        Process p;
        char sub[JSON_ERR_MAX - 48];   /* leaves room for the "process N: " prefix */

        memset(&p, 0, sizeof p);
        if (json_type(o) != JSON_OBJECT) {
            snprintf(err, errsz, "%s %d is not an object", what, i + 1);
            return false;
        }
        if (!json_member_str(o, "id", p.id, sizeof p.id)) {
            snprintf(err, errsz,
                     "%s %d needs an \"id\" string of at most %d "
                     "characters", what, i + 1, PROC_ID_MAX - 1);
            return false;
        }
        if (!json_member_int(o, "arrival", &p.arrival)) {
            snprintf(err, errsz, "%s %s: \"arrival\" must be a whole "
                                 "number", what, p.id);
            return false;
        }
        if (!json_member_int(o, "burst", &p.burst)) {
            snprintf(err, errsz, "%s %s: \"burst\" must be a whole "
                                 "number", what, p.id);
            return false;
        }
        if (json_get(o, "priority") != NULL) {
            if (!json_member_int(o, "priority", &p.priority)) {
                snprintf(err, errsz, "%s %s: \"priority\" must be a "
                                     "whole number", what, p.id);
                return false;
            }
        }

        if (!validate_process(&p, sub, sizeof sub)) {
            snprintf(err, errsz, "%s %d: %s", what, i + 1, sub);
            return false;
        }
        if (dataset_find_id(ds, p.id) >= 0) {
            snprintf(err, errsz, "duplicate process id \"%s\"", p.id);
            return false;
        }
        if (!dataset_add(ds, &p)) {
            snprintf(err, errsz, "out of memory");
            return false;
        }
    }
    return true;
}

static bool read_workload(const JsonValue *root, Dataset *ds,
                          AlgorithmId *algo, int *quantum,
                          char *err, size_t errsz)
{
    const JsonValue *arr;
    char name[32];
    int n;

    dataset_init(ds, "dashboard");

    if (json_type(root) != JSON_OBJECT) {
        snprintf(err, errsz, "the request body must be a JSON object");
        return false;
    }

    if (algo != NULL) {
        if (!json_member_str(root, "algorithm", name, sizeof name)) {
            snprintf(err, errsz, "\"algorithm\" must be a string");
            return false;
        }
        if (!algo_from_name(name, algo)) {
            snprintf(err, errsz,
                     "unknown algorithm \"%s\" (use fcfs, sjf, srtf, "
                     "prio-np, prio-p or rr)", name);
            return false;
        }
    }

    *quantum = DEFAULT_QUANTUM;
    if (json_get(root, "quantum") != NULL) {
        if (!json_member_int(root, "quantum", quantum)) {
            snprintf(err, errsz, "\"quantum\" must be a whole number");
            return false;
        }
        if (!validate_quantum(*quantum, err, errsz)) return false;
    }

    arr = json_get(root, "processes");
    if (json_type(arr) != JSON_ARRAY) {
        snprintf(err, errsz, "\"processes\" must be an array");
        return false;
    }
    n = json_len(arr);
    if (n < 1) {
        snprintf(err, errsz, "add at least one process");
        return false;
    }
    if (!read_processes(arr, ds, "process", err, errsz)) return false;
    if (!validate_dataset(ds, err, errsz)) return false;
    return true;
}

/* Reads the optional forecast settings. Absent keys keep the defaults
 * (inferred window, horizon = window length, no cap, seed 42); a key that
 * is present must be valid -- nothing is silently replaced. */
static bool read_forecast_config(const JsonValue *root, ForecastConfig *cfg,
                                 char *err, size_t errsz)
{
    bool has_start = json_get(root, "observationStart") != NULL;
    bool has_end = json_get(root, "observationEnd") != NULL;
    int v = 0;

    forecast_config_default(cfg);

    if (json_get(root, "horizon") != NULL) {
        if (!json_member_int(root, "horizon", &v)) {
            snprintf(err, errsz, "\"horizon\" must be a whole number");
            return false;
        }
        if (v < 1 || v > MAX_ARRIVAL) {
            snprintf(err, errsz, "the forecast horizon must be between 1 "
                     "and %d", MAX_ARRIVAL);
            return false;
        }
        cfg->horizon = v;
    }

    if (has_start && !has_end) {
        snprintf(err, errsz, "\"observationEnd\" is required when "
                 "\"observationStart\" is given");
        return false;
    }
    if (has_end) {
        cfg->mode = OBS_EXPLICIT;
        cfg->obs_start = MIN_ARRIVAL;     /* the simulator's time origin */
        if (has_start && !json_member_int(root, "observationStart",
                                          &cfg->obs_start)) {
            snprintf(err, errsz, "\"observationStart\" must be a whole "
                     "number");
            return false;
        }
        if (!json_member_int(root, "observationEnd", &cfg->obs_end)) {
            snprintf(err, errsz, "\"observationEnd\" must be a whole number");
            return false;
        }
    }

    if (json_get(root, "maxGenerated") != NULL) {
        if (!json_member_int(root, "maxGenerated", &v)) {
            snprintf(err, errsz, "\"maxGenerated\" must be a whole number");
            return false;
        }
        if (v < 1) {
            snprintf(err, errsz, "the cap on predicted processes must be at "
                     "least 1 (leave it out for no cap)");
            return false;
        }
        cfg->max_generated = v;
    }

    if (json_get(root, "seed") != NULL) {
        if (!json_member_int(root, "seed", &v)) {
            snprintf(err, errsz, "\"seed\" must be a whole number between 0 "
                     "and %ld", PREDICT_MAX_SEED);
            return false;
        }
        if (v < 0) {
            snprintf(err, errsz, "the seed must be between 0 and %ld",
                     PREDICT_MAX_SEED);
            return false;
        }
        cfg->seed = (long)v;
    }
    return true;
}

/* ---------------------------------------------------------- API routes -- */

static void finish_json(Json *j, HttpResponse *res)
{
    if (!json_ok(j)) {
        json_dispose(j);
        http_respond_json_error(res, 500, "out of memory building the reply");
        return;
    }
    /* Hand the buffer straight to the response; no second copy. */
    http_respond_owned(res, 200, JSON_CTYPE, j->buf, j->len);
    j->buf = NULL;            /* ownership transferred */
    j->len = 0;
    j->cap = 0;
}

static void route_algorithms(HttpResponse *res)
{
    Json j;
    int i;

    json_init(&j);
    json_obj_begin(&j);
    json_key(&j, "algorithms");
    json_arr_begin(&j);
    for (i = 0; i < ALGO_COUNT; i++) {
        const AlgorithmInfo *info = algo_info((AlgorithmId)i);
        json_obj_begin(&j);
        json_kv_str(&j, "key", info->abbrev);
        json_kv_str(&j, "name", info->name);
        json_kv_str(&j, "kind", info->kind);
        json_kv_bool(&j, "preemptive", info->preemptive);
        json_kv_bool(&j, "needsQuantum", info->needs_quantum);
        json_kv_str(&j, "summary", info->summary);
        json_kv_str(&j, "rule", info->rule);
        /* The value the API expects back in "algorithm". */
        {
            static const char *const SLUG[ALGO_COUNT] = {
                "fcfs", "sjf", "srtf", "prio-np", "prio-p", "rr"
            };
            json_kv_str(&j, "slug", SLUG[i]);
        }
        json_obj_end(&j);
    }
    json_arr_end(&j);
    json_kv_int(&j, "defaultQuantum", DEFAULT_QUANTUM);
    json_key(&j, "limits");
    json_obj_begin(&j);
    json_kv_int(&j, "maxProcesses", MAX_PROCESSES);
    json_kv_int(&j, "minArrival", MIN_ARRIVAL);
    json_kv_int(&j, "maxArrival", MAX_ARRIVAL);
    json_kv_int(&j, "minBurst", MIN_BURST);
    json_kv_int(&j, "maxBurst", MAX_BURST);
    json_kv_int(&j, "minPriority", MIN_PRIORITY);
    json_kv_int(&j, "maxPriority", MAX_PRIORITY);
    json_kv_int(&j, "minQuantum", MIN_QUANTUM);
    json_kv_int(&j, "maxQuantum", MAX_QUANTUM);
    json_obj_end(&j);
    json_kv_str(&j, "version", SCHEDULIFY_VERSION);
    json_obj_end(&j);

    finish_json(&j, res);
}

static void route_examples(HttpResponse *res)
{
    Json j;
    int e;

    json_init(&j);
    json_obj_begin(&j);
    json_key(&j, "examples");
    json_arr_begin(&j);
    for (e = 0; e < examples_count(); e++) {
        Dataset ds;
        int i;

        if (!example_load(e, &ds)) { dataset_free(&ds); continue; }

        json_obj_begin(&j);
        json_kv_int(&j, "index", e);
        json_kv_str(&j, "name", example_name(e));
        json_kv_str(&j, "description", example_description(e));
        json_kv_int(&j, "quantum", example_quantum(e));
        json_key(&j, "processes");
        json_arr_begin(&j);
        for (i = 0; i < ds.count; i++) {
            json_obj_begin(&j);
            json_kv_str(&j, "id", ds.items[i].id);
            json_kv_int(&j, "arrival", ds.items[i].arrival);
            json_kv_int(&j, "burst", ds.items[i].burst);
            json_kv_int(&j, "priority", ds.items[i].priority);
            json_obj_end(&j);
        }
        json_arr_end(&j);
        json_obj_end(&j);

        dataset_free(&ds);
    }
    json_arr_end(&j);
    json_obj_end(&j);

    finish_json(&j, res);
}

static void route_simulate(const char *body, HttpResponse *res)
{
    JsonValue *root = NULL;
    char err[JSON_ERR_MAX];
    Dataset ds;
    AlgorithmId algo = ALGO_FCFS;
    int quantum = DEFAULT_QUANTUM;
    Schedule sch;
    Json j;

    if (!json_parse(body, &root, err, sizeof err)) {
        http_respond_json_error(res, 400, err);
        return;
    }
    if (!read_workload(root, &ds, &algo, &quantum, err, sizeof err)) {
        json_free(root);
        dataset_free(&ds);
        http_respond_json_error(res, 400, err);
        return;
    }
    json_free(root);

    /* Tracing on: the dashboard shows the scheduler's own reasoning. */
    if (!algo_run(&sch, &ds, algo, quantum, true, err, sizeof err)) {
        schedule_free(&sch);
        dataset_free(&ds);
        http_respond_json_error(res, 400, err);
        return;
    }

    json_init(&j);
    write_schedule(&j, &sch, -1);
    finish_json(&j, res);

    schedule_free(&sch);
    dataset_free(&ds);
}

static void route_compare(const char *body, HttpResponse *res)
{
    JsonValue *root = NULL;
    char err[JSON_ERR_MAX];
    Dataset ds;
    int quantum = DEFAULT_QUANTUM;
    Json j;
    int i;

    if (!json_parse(body, &root, err, sizeof err)) {
        http_respond_json_error(res, 400, err);
        return;
    }
    if (!read_workload(root, &ds, NULL, &quantum, err, sizeof err)) {
        json_free(root);
        dataset_free(&ds);
        http_respond_json_error(res, 400, err);
        return;
    }
    json_free(root);

    json_init(&j);
    json_obj_begin(&j);
    json_kv_int(&j, "quantum", quantum);
    json_key(&j, "results");
    json_arr_begin(&j);

    for (i = 0; i < ALGO_COUNT; i++) {
        Schedule sch;
        char rerr[JSON_ERR_MAX];

        if (algo_run(&sch, &ds, (AlgorithmId)i, quantum, false, rerr,
                     sizeof rerr)) {
            write_schedule(&j, &sch, -1);
        } else {
            json_obj_begin(&j);
            json_key(&j, "algorithm");
            write_algorithm(&j, (AlgorithmId)i, quantum);
            json_kv_str(&j, "error", rerr);
            json_obj_end(&j);
        }
        schedule_free(&sch);
    }

    json_arr_end(&j);
    json_obj_end(&j);
    finish_json(&j, res);

    dataset_free(&ds);
}

/* ---------------------------------------------------------- forecast -- */

static void write_process_list(Json *j, const Dataset *ds, bool predicted)
{
    int i;
    json_arr_begin(j);
    for (i = 0; i < ds->count; i++) {
        json_obj_begin(j);
        json_kv_str(j, "id", ds->items[i].id);
        json_kv_int(j, "arrival", ds->items[i].arrival);
        json_kv_int(j, "burst", ds->items[i].burst);
        json_kv_int(j, "priority", ds->items[i].priority);
        json_kv_bool(j, "predicted", predicted);
        json_obj_end(j);
    }
    json_arr_end(j);
}

/* A p-value or other optional statistic: null when it could not be run. */
static void kv_maybe(Json *j, const char *key, bool valid, double v, int dp)
{
    json_key(j, key);
    if (valid) json_double(j, v, dp);
    else json_null(j);
}

/* The Poisson(lambda*h) mass around its centre, for the count chart. At
 * most 61 bars: for a large mean that is the central band, which is where
 * the mass is. */
static void write_pmf(Json *j, double mu)
{
    int lo = predict_poisson_quantile(0.001, mu);
    int hi = predict_poisson_quantile(0.999, mu);
    int k;

    if (lo < 0 || hi < 0) { json_arr_begin(j); json_arr_end(j); return; }
    if (hi - lo > 60) {
        int centre = (int)mu;
        lo = (centre - 30 > lo) ? centre - 30 : lo;
        hi = lo + 60;
    }
    json_arr_begin(j);
    for (k = lo; k <= hi; k++) {
        json_obj_begin(j);
        json_kv_int(j, "k", k);
        json_kv_double(j, "p", predict_poisson_pmf(k, mu), 6);
        json_obj_end(j);
    }
    json_arr_end(j);
}

static void route_forecast(const char *body, HttpResponse *res)
{
    JsonValue *root = NULL;
    char err[JSON_ERR_MAX];
    Dataset ds, pred;
    ForecastConfig cfg;
    ForecastResult fr;
    int quantum = DEFAULT_QUANTUM;
    Json j;
    int i;

    if (!json_parse(body, &root, err, sizeof err)) {
        http_respond_json_error(res, 400, err);
        return;
    }
    if (!read_workload(root, &ds, NULL, &quantum, err, sizeof err)
        || !read_forecast_config(root, &cfg, err, sizeof err)) {
        json_free(root);
        dataset_free(&ds);
        http_respond_json_error(res, 400, err);
        return;
    }
    json_free(root);

    if (!forecast_run(&ds, &cfg, &pred, &fr, err, sizeof err)) {
        dataset_free(&pred);
        dataset_free(&ds);
        http_respond_json_error(res, 400, err);
        return;
    }

    json_init(&j);
    json_obj_begin(&j);

    json_key(&j, "observation");
    json_obj_begin(&j);
    json_kv_str(&j, "mode", fr.mode == OBS_EXPLICIT ? "explicit"
                                                    : "arrival-gaps");
    json_kv_int(&j, "start", fr.obs_start);
    json_kv_int(&j, "end", fr.obs_end);
    json_kv_int(&j, "duration", fr.exposure);
    json_kv_int(&j, "processes", fr.processes);
    json_kv_int(&j, "events", fr.observed_events);
    json_kv_int(&j, "intervals", fr.intervals);
    kv_maybe(&j, "meanGap", fr.intervals > 0, fr.mean_gap, 4);
    json_kv_str(&j, "assumption", fr.assumption);
    json_obj_end(&j);

    json_key(&j, "rate");
    json_obj_begin(&j);
    json_kv_double(&j, "lambda", fr.lambda, 6);
    json_kv_double(&j, "standardError", fr.lambda_se, 6);
    json_obj_end(&j);

    json_key(&j, "forecast");
    json_obj_begin(&j);
    json_kv_int(&j, "horizon", fr.horizon);
    json_kv_int(&j, "start", fr.forecast_start);
    json_kv_int(&j, "end", fr.forecast_end);
    json_kv_double(&j, "expected", fr.expected, 4);
    json_kv_double(&j, "pZero", fr.p_zero, 6);
    json_kv_double(&j, "pAtLeastOne", fr.p_at_least_one, 6);
    json_key(&j, "range90");
    json_obj_begin(&j);
    json_kv_int(&j, "low", fr.range_low);
    json_kv_int(&j, "high", fr.range_high);
    json_obj_end(&j);
    json_key(&j, "pmf");
    write_pmf(&j, fr.expected);
    json_kv_int(&j, "cap", fr.cap);
    json_kv_bool(&j, "capFromUser", fr.cap_from_user);
    json_kv_double(&j, "pCapExceeded", fr.p_cap_exceeded, 6);
    json_kv_int(&j, "generated", fr.generated);
    json_kv_bool(&j, "truncated", fr.truncated);
    json_kv_int(&j, "tiedArrivals", fr.tied_arrivals);
    json_obj_end(&j);

    json_key(&j, "bursts");
    json_obj_begin(&j);
    json_kv_double(&j, "mean", fr.burst_mean, 4);
    kv_maybe(&j, "sd", fr.burst_sd_valid, fr.burst_sd, 4);
    json_kv_int(&j, "min", fr.burst_min);
    json_kv_int(&j, "max", fr.burst_max);
    json_kv_int(&j, "distinct", fr.burst_distinct);
    json_kv_int(&j, "samples", fr.processes);
    json_obj_end(&j);

    json_key(&j, "priorities");
    json_obj_begin(&j);
    json_kv_int(&j, "min", fr.priority_min);
    json_kv_int(&j, "max", fr.priority_max);
    json_kv_int(&j, "distinct", fr.priority_distinct);
    json_obj_end(&j);

    json_key(&j, "stability");
    json_obj_begin(&j);
    json_kv_int(&j, "simultaneousArrivals", fr.simultaneous);
    json_kv_int(&j, "testEvents", fr.test_events);
    json_key(&j, "split");
    json_obj_begin(&j);
    json_kv_bool(&j, "valid", fr.split_valid);
    json_kv_int(&j, "minEvents", PREDICT_SPLIT_MIN);
    if (fr.split_valid) {
        json_kv_int(&j, "firstCount", fr.split_first);
        json_kv_int(&j, "secondCount", fr.split_second);
        json_kv_double(&j, "firstRate", fr.split_rate_first, 6);
        json_kv_double(&j, "secondRate", fr.split_rate_second, 6);
        json_kv_double(&j, "pValue", fr.split_p, 6);
    }
    json_obj_end(&j);
    json_key(&j, "dispersion");
    json_obj_begin(&j);
    json_kv_bool(&j, "valid", fr.disp_valid);
    json_kv_int(&j, "minEvents", PREDICT_DISP_MIN);
    if (fr.disp_valid) {
        json_kv_int(&j, "bins", fr.disp_bins);
        json_kv_double(&j, "varianceToMean", fr.disp_vmr, 4);
        json_kv_double(&j, "pOver", fr.disp_p_over, 6);
        json_kv_double(&j, "pUnder", fr.disp_p_under, 6);
    }
    json_obj_end(&j);
    json_obj_end(&j);

    json_kv_int(&j, "seed", fr.seed);

    json_key(&j, "warnings");
    json_arr_begin(&j);
    for (i = 0; i < fr.warn_count; i++) json_str(&j, fr.warnings[i]);
    json_arr_end(&j);

    json_key(&j, "predicted");
    write_process_list(&j, &pred, true);

    json_obj_end(&j);
    finish_json(&j, res);

    dataset_free(&pred);
    dataset_free(&ds);
}

/* Averages over the first n processes only -- how the user's own processes
 * fare once the forecast arrivals compete with them. */
static void subset_averages(const Schedule *sch, int n, double *wt,
                            double *tat, double *rt)
{
    long w = 0, t = 0, r = 0;
    int i;
    for (i = 0; i < n && i < sch->n; i++) {
        w += sch->stats[i].waiting;
        t += sch->stats[i].turnaround;
        r += sch->stats[i].response;
    }
    *wt = (n > 0) ? (double)w / n : 0.0;
    *tat = (n > 0) ? (double)t / n : 0.0;
    *rt = (n > 0) ? (double)r / n : 0.0;
}

static void write_comparison_row(Json *j, const char *key, const char *label,
                                 double before, double after, int dp,
                                 const char *better)
{
    json_obj_begin(j);
    json_kv_str(j, "key", key);
    json_kv_str(j, "label", label);
    json_kv_double(j, "original", before, dp);
    json_kv_double(j, "augmented", after, dp);
    json_kv_double(j, "change", after - before, dp);
    json_kv_str(j, "better", better);
    json_kv_int(j, "decimals", dp);
    json_obj_end(j);
}

static void route_forecast_simulate(const char *body, HttpResponse *res)
{
    JsonValue *root = NULL;
    const JsonValue *parr;
    char err[JSON_ERR_MAX];
    Dataset ds, pred, merged;
    AlgorithmId algo = ALGO_FCFS;
    int quantum = DEFAULT_QUANTUM;
    Schedule orig, aug;
    double ow, ot, orr, aw, at, ar;
    Json j;

    dataset_init(&pred, "forecast");
    dataset_init(&merged, "observed + forecast");
    memset(&orig, 0, sizeof orig);
    memset(&aug, 0, sizeof aug);

    if (!json_parse(body, &root, err, sizeof err)) {
        http_respond_json_error(res, 400, err);
        return;
    }
    if (!read_workload(root, &ds, &algo, &quantum, err, sizeof err)) {
        goto bad;
    }
    parr = json_get(root, "predicted");
    if (json_type(parr) != JSON_ARRAY) {
        snprintf(err, sizeof err, "\"predicted\" must be an array (generate "
                 "a forecast first)");
        goto bad;
    }
    if (!read_processes(parr, &pred, "predicted process", err, sizeof err)
        || !forecast_merge(&ds, &pred, &merged, err, sizeof err)) {
        goto bad;
    }
    json_free(root);
    root = NULL;

    /* The unchanged engine, twice: the user's workload and the same
     * workload with the forecast arrivals appended. */
    if (!algo_run(&orig, &ds, algo, quantum, true, err, sizeof err)
        || !algo_run(&aug, &merged, algo, quantum, true, err, sizeof err)) {
        goto bad;
    }

    subset_averages(&orig, ds.count, &ow, &ot, &orr);
    subset_averages(&aug, ds.count, &aw, &at, &ar);

    json_init(&j);
    json_obj_begin(&j);
    json_kv_int(&j, "originalCount", ds.count);
    json_kv_int(&j, "predictedCount", pred.count);

    json_key(&j, "original");
    write_schedule(&j, &orig, -1);
    json_key(&j, "augmented");
    write_schedule(&j, &aug, ds.count);

    json_key(&j, "comparison");
    json_arr_begin(&j);
    write_comparison_row(&j, "avgWaiting", "Avg waiting (all processes)",
                         orig.avg_waiting, aug.avg_waiting, 2, "lower");
    write_comparison_row(&j, "avgTurnaround",
                         "Avg turnaround (all processes)",
                         orig.avg_turnaround, aug.avg_turnaround, 2,
                         "lower");
    write_comparison_row(&j, "avgResponse", "Avg response (all processes)",
                         orig.avg_response, aug.avg_response, 2, "lower");
    write_comparison_row(&j, "origAvgWaiting",
                         "Avg waiting (your original processes)",
                         ow, aw, 2, "lower");
    write_comparison_row(&j, "origAvgTurnaround",
                         "Avg turnaround (your original processes)",
                         ot, at, 2, "lower");
    write_comparison_row(&j, "origAvgResponse",
                         "Avg response (your original processes)",
                         orr, ar, 2, "lower");
    write_comparison_row(&j, "cpuUtilization", "CPU utilisation %",
                         orig.cpu_utilization, aug.cpu_utilization, 2,
                         "higher");
    write_comparison_row(&j, "throughput", "Throughput (per unit)",
                         orig.throughput, aug.throughput, 4, "higher");
    write_comparison_row(&j, "contextSwitches", "Context switches",
                         orig.context_switches, aug.context_switches, 0,
                         "lower");
    write_comparison_row(&j, "preemptions", "Preemptions",
                         orig.preemptions, aug.preemptions, 0, "lower");
    write_comparison_row(&j, "idleTime", "CPU idle (units)",
                         orig.idle_time, aug.idle_time, 0, "lower");
    write_comparison_row(&j, "totalTime", "Makespan (units)",
                         orig.total_time, aug.total_time, 0, "lower");
    write_comparison_row(&j, "processes", "Processes",
                         orig.n, aug.n, 0, "none");
    json_arr_end(&j);

    json_obj_end(&j);
    finish_json(&j, res);

    schedule_free(&orig);
    schedule_free(&aug);
    dataset_free(&merged);
    dataset_free(&pred);
    dataset_free(&ds);
    return;

bad:
    json_free(root);
    schedule_free(&orig);
    schedule_free(&aug);
    dataset_free(&merged);
    dataset_free(&pred);
    dataset_free(&ds);
    http_respond_json_error(res, 400, err);
}

/* ------------------------------------------------------------- dispatch */

void webapp_route(WebApp *app, const char *method, const char *path,
                  const char *body, HttpResponse *res)
{
    bool is_get = (strcmp(method, "GET") == 0);
    bool is_post = (strcmp(method, "POST") == 0);

    if (strcmp(path, "/api/algorithms") == 0) {
        if (!is_get) { http_respond_json_error(res, 405, "use GET"); return; }
        route_algorithms(res);
        return;
    }
    if (strcmp(path, "/api/examples") == 0) {
        if (!is_get) { http_respond_json_error(res, 405, "use GET"); return; }
        route_examples(res);
        return;
    }
    if (strcmp(path, "/api/simulate") == 0) {
        if (!is_post) { http_respond_json_error(res, 405, "use POST"); return; }
        route_simulate(body, res);
        return;
    }
    if (strcmp(path, "/api/compare") == 0) {
        if (!is_post) { http_respond_json_error(res, 405, "use POST"); return; }
        route_compare(body, res);
        return;
    }
    if (strcmp(path, "/api/forecast") == 0) {
        if (!is_post) { http_respond_json_error(res, 405, "use POST"); return; }
        route_forecast(body, res);
        return;
    }
    if (strcmp(path, "/api/forecast/simulate") == 0) {
        if (!is_post) { http_respond_json_error(res, 405, "use POST"); return; }
        route_forecast_simulate(body, res);
        return;
    }

    if (!is_get) {
        http_respond_json_error(res, 405, "use GET");
        return;
    }
    if (app == NULL || !app->have_root) {
        http_respond_json_error(res, 500, "the web root is not available");
        return;
    }
    if (strcmp(path, "/") == 0) {
        serve_static(app, "index.html", res);
        return;
    }
    serve_static(app, path + 1, res);     /* skip the leading '/' */
}

void webapp_handle(const HttpRequest *req, HttpResponse *res, void *ctx)
{
    webapp_route((WebApp *)ctx, req->method, req->path,
                 (req->body != NULL) ? req->body : "", res);
}
