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

static void write_schedule(Json *j, const Schedule *sch)
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

static bool read_workload(const JsonValue *root, Dataset *ds,
                          AlgorithmId *algo, int *quantum,
                          char *err, size_t errsz)
{
    const JsonValue *arr;
    char name[32];
    int n, i;

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
    if (n > MAX_PROCESSES) {
        snprintf(err, errsz, "at most %d processes are supported",
                 MAX_PROCESSES);
        return false;
    }

    for (i = 0; i < n; i++) {
        const JsonValue *o = json_at(arr, i);
        Process p;
        char sub[JSON_ERR_MAX - 32];   /* leaves room for the "process N: " prefix */

        memset(&p, 0, sizeof p);
        if (json_type(o) != JSON_OBJECT) {
            snprintf(err, errsz, "process %d is not an object", i + 1);
            return false;
        }
        if (!json_member_str(o, "id", p.id, sizeof p.id)) {
            snprintf(err, errsz,
                     "process %d needs an \"id\" string of at most %d "
                     "characters", i + 1, PROC_ID_MAX - 1);
            return false;
        }
        if (!json_member_int(o, "arrival", &p.arrival)) {
            snprintf(err, errsz, "process %s: \"arrival\" must be a whole "
                                 "number", p.id);
            return false;
        }
        if (!json_member_int(o, "burst", &p.burst)) {
            snprintf(err, errsz, "process %s: \"burst\" must be a whole "
                                 "number", p.id);
            return false;
        }
        if (json_get(o, "priority") != NULL) {
            if (!json_member_int(o, "priority", &p.priority)) {
                snprintf(err, errsz, "process %s: \"priority\" must be a "
                                     "whole number", p.id);
                return false;
            }
        }

        /* The same predicates the keyboard and the CSV importer use. */
        if (!validate_process(&p, sub, sizeof sub)) {
            snprintf(err, errsz, "process %d: %s", i + 1, sub);
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

    if (!validate_dataset(ds, err, errsz)) return false;
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
    write_schedule(&j, &sch);
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
            write_schedule(&j, &sch);
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
