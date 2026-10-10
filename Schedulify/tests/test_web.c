/*
 * test_web.c -- The JSON codec and the dashboard's HTTP routes.
 *
 * The routes are driven through webapp_route() directly: no socket, no
 * browser, no timing. What this suite is really protecting is the promise
 * that the browser is shown exactly what the C engine computed -- so the
 * numbers asserted here are the same hand-worked ones the algorithm suite
 * checks, read back out of the JSON.
 */
#include "suites.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithms.h"
#include "datasets.h"
#include "http.h"
#include "json.h"
#include "webapp.h"

/* ───────────────────────────── the reader ───────────────────────── */

static void test_json_reading(void)
{
    JsonValue *v = NULL;
    char err[JSON_ERR_MAX];

    t_case("JSON reader: shapes and types");

    CHECK(json_parse("{\"a\":1,\"b\":\"two\",\"c\":true,\"d\":null,"
                     "\"e\":[1,2,3]}", &v, err, sizeof err));
    if (v != NULL) {
        int n = 0;
        bool b = false;
        CHECK_INT(json_type(v), JSON_OBJECT);
        CHECK(json_member_int(v, "a", &n));   CHECK_INT(n, 1);
        CHECK_STR(json_as_string(json_get(v, "b")), "two");
        CHECK(json_member_bool(v, "c", &b));  CHECK(b);
        CHECK_INT(json_type(json_get(v, "d")), JSON_NULL);
        CHECK_INT(json_len(json_get(v, "e")), 3);
        CHECK(json_as_int(json_at(json_get(v, "e"), 2), &n)); CHECK_INT(n, 3);
        CHECK(json_get(v, "missing") == NULL);
        CHECK(json_at(json_get(v, "e"), 9) == NULL);
        json_free(v);
    }

    /* negative and signed numbers */
    CHECK(json_parse("{\"n\":-17,\"z\":0}", &v, err, sizeof err));
    if (v != NULL) {
        int n = 99;
        CHECK(json_member_int(v, "n", &n)); CHECK_INT(n, -17);
        CHECK(json_member_int(v, "z", &n)); CHECK_INT(n, 0);
        json_free(v);
    }

    /* escapes, including \u */
    CHECK(json_parse("{\"s\":\"a\\\"b\\\\c\\nd\\u0041\"}", &v, err,
                     sizeof err));
    if (v != NULL) {
        CHECK_STR(json_as_string(json_get(v, "s")), "a\"b\\c\ndA");
        json_free(v);
    }

    /* whitespace everywhere */
    CHECK(json_parse("  {  \"a\" : [ 1 , 2 ] }  ", &v, err, sizeof err));
    json_free(v);

    t_case("JSON reader: integers are integers");
    /* 1e300 is a perfectly good double but far outside int range, so the
     * parse succeeds and only the int extraction refuses it. */
    CHECK(json_parse("{\"a\":2.5,\"b\":42,\"c\":1e300,\"d\":-1e300}",
                     &v, err, sizeof err));
    if (v != NULL) {
        int n = 0;
        double d = 0.0;
        CHECK(!json_as_int(json_get(v, "a"), &n));   /* 2.5 is not an int */
        CHECK(json_as_double(json_get(v, "a"), &d)); CHECK_DBL(d, 2.5);
        CHECK(json_as_int(json_get(v, "b"), &n));    CHECK_INT(n, 42);
        CHECK(!json_as_int(json_get(v, "c"), &n));   /* above INT_MAX     */
        CHECK(!json_as_int(json_get(v, "d"), &n));   /* below INT_MIN     */
        CHECK(!json_as_int(json_get(v, "missing"), &n));
        json_free(v);
    }

    /* A number too large for a double at all is refused outright, rather
     * than silently becoming infinity. */
    v = NULL;
    CHECK(!json_parse("{\"c\":1e400}", &v, err, sizeof err));
    CHECK(strstr(err, "range") != NULL);
    json_free(v);
}

static void test_json_rejects(void)
{
    JsonValue *v = NULL;
    char err[JSON_ERR_MAX];
    size_t i;
    static const char *const BAD[] = {
        "{",                       /* unterminated object      */
        "{\"a\":}",                /* missing value            */
        "{\"a\" 1}",               /* missing colon            */
        "{\"a\":1,}",              /* trailing comma           */
        "[1,2",                    /* unterminated array       */
        "\"unterminated",          /* unterminated string      */
        "{\"a\":\"\\q\"}",         /* unknown escape           */
        "{\"a\":1} trailing",      /* junk after the value     */
        "nope",                    /* not a literal            */
        ""                         /* empty                    */
    };

    t_case("JSON reader: malformed input is rejected, not guessed");
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        v = NULL;
        CHECK_MSG(!json_parse(BAD[i], &v, err, sizeof err),
                  "should have rejected: %s", BAD[i]);
        CHECK_MSG(v == NULL, "a rejected parse must not leak a tree");
        CHECK_MSG(err[0] != '\0', "a rejection must explain itself");
        json_free(v);
    }

    /* A NULL body is a rejection, not a crash. */
    v = NULL;
    CHECK(!json_parse(NULL, &v, err, sizeof err));
    CHECK(v == NULL);

    /* Nesting deeper than the limit is refused rather than overflowing. */
    {
        char deep[4 * JSON_MAX_DEPTH + 8];
        size_t k;
        for (k = 0; k < 2 * JSON_MAX_DEPTH; k++) deep[k] = '[';
        deep[2 * JSON_MAX_DEPTH] = '\0';
        v = NULL;
        CHECK(!json_parse(deep, &v, err, sizeof err));
        json_free(v);
    }
}

/* ───────────────────────────── the writer ───────────────────────── */

static void test_json_writing(void)
{
    Json j;

    t_case("JSON writer: punctuation and nesting");

    json_init(&j);
    json_obj_begin(&j);
    json_kv_str(&j, "name", "P1");
    json_kv_int(&j, "burst", 7);
    json_kv_bool(&j, "idle", false);
    json_kv_double(&j, "avg", 7.0, 2);
    json_key(&j, "list");
    json_arr_begin(&j);
    json_int(&j, 1);
    json_int(&j, 2);
    json_obj_begin(&j);
    json_kv_int(&j, "x", 9);
    json_obj_end(&j);
    json_arr_end(&j);
    json_obj_end(&j);

    CHECK(json_ok(&j));
    CHECK_STR(json_text(&j),
              "{\"name\":\"P1\",\"burst\":7,\"idle\":false,\"avg\":7.00,"
              "\"list\":[1,2,{\"x\":9}]}");
    json_dispose(&j);

    t_case("JSON writer: strings are escaped");
    json_init(&j);
    json_arr_begin(&j);
    json_str(&j, "quote\" back\\ tab\t nl\n");
    json_arr_end(&j);
    CHECK_STR(json_text(&j),
              "[\"quote\\\" back\\\\ tab\\t nl\\n\"]");
    json_dispose(&j);

    t_case("JSON writer: round trip through the reader");
    {
        JsonValue *v = NULL;
        char err[JSON_ERR_MAX];
        int n = 0;

        json_init(&j);
        json_obj_begin(&j);
        json_kv_str(&j, "msg", "he said \"hi\"\nthen left");
        json_kv_int(&j, "n", -5);
        json_obj_end(&j);

        CHECK(json_parse(json_text(&j), &v, err, sizeof err));
        if (v != NULL) {
            CHECK_STR(json_as_string(json_get(v, "msg")),
                      "he said \"hi\"\nthen left");
            CHECK(json_member_int(v, "n", &n));
            CHECK_INT(n, -5);
            json_free(v);
        }
        json_dispose(&j);
    }
}

/* ───────────────────────────── the routes ──────────────────────── */

/* Calls a route and parses the reply.  The caller owns *out. */
static int route(const char *method, const char *path, const char *body,
                 JsonValue **out)
{
    WebApp app;
    HttpResponse res;
    char err[JSON_ERR_MAX];
    int status;

    memset(&app, 0, sizeof app);
    app.have_root = true;
    snprintf(app.web_root, sizeof app.web_root, "web");

    memset(&res, 0, sizeof res);
    res.status = 200;

    webapp_route(&app, method, path, body, &res);
    status = res.status;

    *out = NULL;
    if (res.body != NULL) (void)json_parse(res.body, out, err, sizeof err);
    if (res.body_owned) free(res.body);
    return status;
}

static void test_route_algorithms(void)
{
    JsonValue *v = NULL;
    int status, i;

    t_case("GET /api/algorithms lists all six with their rules");
    status = route("GET", "/api/algorithms", "", &v);
    CHECK_INT(status, 200);

    if (v != NULL) {
        const JsonValue *arr = json_get(v, "algorithms");
        CHECK_INT(json_len(arr), ALGO_COUNT);
        for (i = 0; i < json_len(arr); i++) {
            const JsonValue *a = json_at(arr, i);
            CHECK_MSG(json_as_string(json_get(a, "slug")) != NULL,
                      "algorithm %d has no slug", i);
            CHECK_MSG(json_as_string(json_get(a, "rule")) != NULL,
                      "algorithm %d has no documented rule", i);
            CHECK_STR(json_as_string(json_get(a, "key")),
                      algo_abbrev((AlgorithmId)i));
        }
        /* The limits the editor clamps against come from the C headers. */
        {
            int n = 0;
            const JsonValue *lim = json_get(v, "limits");
            CHECK(json_member_int(lim, "maxProcesses", &n));
            CHECK_INT(n, MAX_PROCESSES);
            CHECK(json_member_int(lim, "minBurst", &n));
            CHECK_INT(n, MIN_BURST);
        }
        json_free(v);
    }

    t_case("method guards");
    CHECK_INT(route("POST", "/api/algorithms", "", &v), 405);
    json_free(v);
    CHECK_INT(route("GET", "/api/simulate", "", &v), 405);
    json_free(v);
    CHECK_INT(route("GET", "/api/compare", "", &v), 405);
    json_free(v);
}

static void test_route_examples(void)
{
    JsonValue *v = NULL;
    int status;

    t_case("GET /api/examples serves the predefined workloads");
    status = route("GET", "/api/examples", "", &v);
    CHECK_INT(status, 200);

    if (v != NULL) {
        const JsonValue *arr = json_get(v, "examples");
        CHECK_INT(json_len(arr), examples_count());
        if (json_len(arr) > 0) {
            const JsonValue *e = json_at(arr, 1);   /* Textbook SJF / SRTF */
            int q = 0;
            CHECK_STR(json_as_string(json_get(e, "name")), example_name(1));
            CHECK(json_member_int(e, "quantum", &q));
            CHECK_INT(q, example_quantum(1));
            CHECK_INT(json_len(json_get(e, "processes")), 4);
        }
        json_free(v);
    }
}

/* The classic workload, as the browser would send it. */
#define CLASSIC_BODY \
    "{\"algorithm\":\"srtf\",\"processes\":[" \
    "{\"id\":\"P1\",\"arrival\":0,\"burst\":7,\"priority\":2}," \
    "{\"id\":\"P2\",\"arrival\":2,\"burst\":4,\"priority\":1}," \
    "{\"id\":\"P3\",\"arrival\":4,\"burst\":1,\"priority\":3}," \
    "{\"id\":\"P4\",\"arrival\":5,\"burst\":4,\"priority\":2}]}"

/* Compares a metric the engine computed against the hand-worked value. */
static void check_double(const JsonValue *obj, const char *key, double want)
{
    double got = 0.0;
    if (!json_member_double(obj, key, &got)) {
        t_check(false, __FILE__, __LINE__, "%s is missing or not a number",
                key);
        return;
    }
    t_check_dbl(got, want, __FILE__, __LINE__, key);
}

static void test_route_simulate(void)
{
    JsonValue *v = NULL;
    int status;

    t_case("POST /api/simulate returns the schedule the C engine computed");
    status = route("POST", "/api/simulate", CLASSIC_BODY, &v);
    CHECK_INT(status, 200);
    if (v == NULL) return;

    /* the timeline, exactly as the algorithm suite asserts it */
    {
        const JsonValue *segs = json_get(v, "segments");
        char line[256];
        size_t used = 0;
        int i;

        line[0] = '\0';
        for (i = 0; i < json_len(segs); i++) {
            const JsonValue *s = json_at(segs, i);
            int a = 0, b = 0;
            json_as_int(json_get(s, "start"), &a);
            json_as_int(json_get(s, "end"), &b);
            used += (size_t)snprintf(line + used, sizeof line - used,
                                     "%s%s:%d-%d", (i > 0) ? "," : "",
                                     json_as_string(json_get(s, "id")), a, b);
        }
        CHECK_STR(line, "P1:0-2,P2:2-4,P3:4-5,P2:5-7,P4:7-11,P1:11-16");
    }

    /* per-process numbers */
    {
        const JsonValue *procs = json_get(v, "processes");
        int i, n = 0;
        CHECK_INT(json_len(procs), 4);
        for (i = 0; i < json_len(procs); i++) {
            const JsonValue *p = json_at(procs, i);
            const char *id = json_as_string(json_get(p, "id"));
            if (id == NULL) continue;
            if (strcmp(id, "P1") == 0) {
                CHECK(json_member_int(p, "completion", &n)); CHECK_INT(n, 16);
                CHECK(json_member_int(p, "turnaround", &n)); CHECK_INT(n, 16);
                CHECK(json_member_int(p, "waiting", &n));    CHECK_INT(n, 9);
                CHECK(json_member_int(p, "response", &n));   CHECK_INT(n, 0);
            } else if (strcmp(id, "P4") == 0) {
                CHECK(json_member_int(p, "completion", &n)); CHECK_INT(n, 11);
                CHECK(json_member_int(p, "waiting", &n));    CHECK_INT(n, 2);
                CHECK(json_member_int(p, "response", &n));   CHECK_INT(n, 2);
            }
        }
    }

    /* aggregate metrics */
    {
        const JsonValue *m = json_get(v, "metrics");
        int n = 0;
        CHECK(json_member_int(m, "contextSwitches", &n)); CHECK_INT(n, 5);
        CHECK(json_member_int(m, "preemptions", &n));     CHECK_INT(n, 2);
        CHECK(json_member_int(m, "totalTime", &n));       CHECK_INT(n, 16);
        CHECK(json_member_int(m, "idleTime", &n));        CHECK_INT(n, 0);
        CHECK(json_member_int(m, "busyTime", &n));        CHECK_INT(n, 16);
        CHECK(json_member_int(m, "processes", &n));       CHECK_INT(n, 4);
        check_double(m, "avgTurnaround", 7.0);
        check_double(m, "avgWaiting", 3.0);
        check_double(m, "avgResponse", 0.5);
        check_double(m, "avgCompletion", 9.75);
        check_double(m, "cpuUtilization", 100.0);
        check_double(m, "throughput", 0.25);
    }

    /* the window and the scheduler's trace both travel to the browser */
    {
        const JsonValue *w = json_get(v, "window");
        int n = 0;
        CHECK(json_member_int(w, "start", &n)); CHECK_INT(n, 0);
        CHECK(json_member_int(w, "end", &n));   CHECK_INT(n, 16);
        CHECK_MSG(json_len(json_get(v, "trace")) > 0,
                  "the trace must reach the dashboard");
        CHECK_STR(json_as_string(json_get(json_get(v, "algorithm"), "key")),
                  "SRTF");
    }

    json_free(v);
}

static void test_route_simulate_rejects(void)
{
    JsonValue *v = NULL;
    size_t i;
    static const struct { const char *body; const char *needle; } BAD[] = {
        { "{\"algorithm\":\"nope\",\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":0,\"burst\":1}]}", "unknown algorithm" },
        { "{\"algorithm\":\"fcfs\",\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":0,\"burst\":0}]}", "burst" },
        { "{\"algorithm\":\"fcfs\",\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":-1,\"burst\":2}]}", "arrival" },
        { "{\"algorithm\":\"fcfs\",\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":0,\"burst\":2.5}]}", "whole number" },
        { "{\"algorithm\":\"fcfs\",\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":0,\"burst\":2},"
          "{\"id\":\"P1\",\"arrival\":1,\"burst\":2}]}", "duplicate" },
        { "{\"algorithm\":\"fcfs\",\"processes\":[]}", "at least one" },
        { "{\"algorithm\":\"fcfs\"}", "processes" },
        { "{\"processes\":[{\"id\":\"P1\",\"arrival\":0,\"burst\":1}]}",
          "algorithm" },
        { "{\"algorithm\":\"rr\",\"quantum\":0,\"processes\":"
          "[{\"id\":\"P1\",\"arrival\":0,\"burst\":1}]}", "quantum" },
        { "not json at all", "" },
        { "[1,2,3]", "object" }
    };

    t_case("POST /api/simulate rejects bad workloads with a reason");
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        int status = route("POST", "/api/simulate", BAD[i].body, &v);
        const char *msg = (v != NULL) ? json_as_string(json_get(v, "error"))
                                      : NULL;
        CHECK_MSG(status == 400, "expected 400 for: %s", BAD[i].body);
        CHECK_MSG(msg != NULL, "no error message for: %s", BAD[i].body);
        if (msg != NULL && BAD[i].needle[0] != '\0') {
            CHECK_MSG(strstr(msg, BAD[i].needle) != NULL,
                      "message \"%s\" should mention \"%s\"", msg,
                      BAD[i].needle);
        }
        json_free(v);
        v = NULL;
    }
}

static void test_route_compare(void)
{
    JsonValue *v = NULL;
    int status, i;

    t_case("POST /api/compare returns all six, each with a timeline");
    status = route("POST", "/api/compare",
                   "{\"quantum\":2,\"processes\":["
                   "{\"id\":\"P1\",\"arrival\":0,\"burst\":5},"
                   "{\"id\":\"P2\",\"arrival\":1,\"burst\":4},"
                   "{\"id\":\"P3\",\"arrival\":2,\"burst\":2},"
                   "{\"id\":\"P4\",\"arrival\":4,\"burst\":1}]}", &v);
    CHECK_INT(status, 200);
    if (v == NULL) return;

    {
        const JsonValue *arr = json_get(v, "results");
        CHECK_INT(json_len(arr), ALGO_COUNT);
        for (i = 0; i < json_len(arr); i++) {
            const JsonValue *r = json_at(arr, i);
            CHECK_MSG(json_get(r, "error") == NULL,
                      "%s failed in the comparison",
                      algo_abbrev((AlgorithmId)i));
            CHECK_MSG(json_len(json_get(r, "segments")) > 0,
                      "%s returned no timeline",
                      algo_abbrev((AlgorithmId)i));
            CHECK_MSG(json_get(r, "metrics") != NULL,
                      "%s returned no metrics", algo_abbrev((AlgorithmId)i));
        }

        /* Round Robin is last, and its queue discipline is the one most
         * often got wrong -- assert the exact timeline. */
        {
            const JsonValue *segs = json_get(json_at(arr, ALGO_RR),
                                             "segments");
            char line[256];
            size_t used = 0;
            line[0] = '\0';
            for (i = 0; i < json_len(segs); i++) {
                const JsonValue *s = json_at(segs, i);
                int a = 0, b = 0;
                json_as_int(json_get(s, "start"), &a);
                json_as_int(json_get(s, "end"), &b);
                used += (size_t)snprintf(line + used, sizeof line - used,
                                         "%s%s:%d-%d", (i > 0) ? "," : "",
                                         json_as_string(json_get(s, "id")),
                                         a, b);
            }
            CHECK_STR(line,
                      "P1:0-2,P2:2-4,P3:4-6,P1:6-8,P4:8-9,P2:9-11,P1:11-12");
        }
    }
    json_free(v);
}

static void test_route_static_guards(void)
{
    JsonValue *v = NULL;

    t_case("static files: traversal and odd extensions are refused");

    /* path_is_safe runs before any file is opened, so these are refused
     * whether or not a web root happens to exist. */
    CHECK_INT(route("GET", "/../Makefile", "", &v), 404);        json_free(v);
    CHECK_INT(route("GET", "/../../etc/passwd", "", &v), 404);   json_free(v);
    CHECK_INT(route("GET", "/..%2fMakefile", "", &v), 404);      json_free(v);
    CHECK_INT(route("GET", "/C:/Windows/win.ini", "", &v), 404); json_free(v);
    CHECK_INT(route("GET", "/secrets.env", "", &v), 404);        json_free(v);
    CHECK_INT(route("GET", "/app.exe", "", &v), 404);            json_free(v);
    CHECK_INT(route("GET", "/no-extension", "", &v), 404);       json_free(v);
    CHECK_INT(route("GET", "/nope.js", "", &v), 404);            json_free(v);
}

static void test_url_decode(void)
{
    char a[64];

    t_case("percent-decoding");
    snprintf(a, sizeof a, "/a%%20b");       http_url_decode(a);
    CHECK_STR(a, "/a b");
    snprintf(a, sizeof a, "/%%2e%%2e/x");   http_url_decode(a);
    CHECK_STR(a, "/../x");
    snprintf(a, sizeof a, "a+b");           http_url_decode(a);
    CHECK_STR(a, "a b");
    snprintf(a, sizeof a, "%%zz");          http_url_decode(a);
    CHECK_STR(a, "%zz");                    /* malformed stays literal */
    snprintf(a, sizeof a, "plain");         http_url_decode(a);
    CHECK_STR(a, "plain");
}

void suite_web(void)
{
    t_suite("web");
    test_json_reading();
    test_json_rejects();
    test_json_writing();
    test_route_algorithms();
    test_route_examples();
    test_route_simulate();
    test_route_simulate_rejects();
    test_route_compare();
    test_route_static_guards();
    test_url_decode();
}
