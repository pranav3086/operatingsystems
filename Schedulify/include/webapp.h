/*
 * webapp.h -- The dashboard's routes.
 *
 * This is the only bridge between the browser and the simulator, and it is
 * a thin one.  A request arrives as JSON, is validated by the very same
 * validate.c predicates the keyboard and the CSV importer use, is handed to
 * the unchanged C schedulers, and the resulting Schedule is serialised --
 * timeline, per-process stats, aggregate metrics and the scheduler's own
 * decision trace.
 *
 * The browser receives a finished schedule and animates it.  No scheduling
 * decision, and no metric, is ever computed in JavaScript.
 *
 * Routes
 *   GET  /                 the dashboard page
 *   GET  /<file>           static assets from the web root (whitelisted)
 *   GET  /api/algorithms   the algorithm registry, incl. the tie-break rules
 *   GET  /api/examples     the predefined workloads
 *   POST /api/simulate     one algorithm   -> one result
 *   POST /api/compare      all six         -> six results
 *
 * Request body for both POST routes:
 *   { "algorithm": "srtf", "quantum": 2,
 *     "processes": [ { "id":"P1","arrival":0,"burst":7,"priority":2 }, ... ] }
 */
#ifndef SCHEDULIFY_WEBAPP_H
#define SCHEDULIFY_WEBAPP_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"

#define WEB_ROOT_MAX 512

typedef struct {
    char web_root[WEB_ROOT_MAX];
    bool have_root;
} WebApp;

/* Locates the directory holding index.html.  Tries, in order: an explicit
 * root, ./web, ../web, <exe dir>/web and <exe dir>/../web -- so the server
 * starts from the project directory or from anywhere else.  Returns false
 * with a reason when none of them contains the page. */
bool webapp_init(WebApp *app, const char *explicit_root, const char *argv0,
                 char *err, size_t errsz);

/* The HttpHandler for http_serve(); ctx must be the WebApp. */
void webapp_handle(const HttpRequest *req, HttpResponse *res, void *ctx);

/* Exposed for the unit tests, which drive the routes without a socket. */
void webapp_route(WebApp *app, const char *method, const char *path,
                  const char *body, HttpResponse *res);

#endif /* SCHEDULIFY_WEBAPP_H */
