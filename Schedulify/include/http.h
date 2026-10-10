/*
 * http.h -- A very small HTTP/1.1 server, enough to host the dashboard.
 *
 * Deliberately minimal and deliberately local: the listening socket is bound
 * to 127.0.0.1 only, so the dashboard is reachable from this machine and is
 * never exposed on the network.  One request is handled at a time and every
 * response closes the connection, which is plenty for a single browser tab
 * talking to a simulator that answers in microseconds.
 *
 * Portability: Winsock2 on Windows, BSD sockets everywhere else.
 */
#ifndef SCHEDULIFY_HTTP_H
#define SCHEDULIFY_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_REQUEST  (1024u * 1024u)   /* refuse anything larger */
#define HTTP_METHOD_MAX   16
#define HTTP_PATH_MAX     512
#define HTTP_QUERY_MAX    256
#define HTTP_ERR_MAX      200

typedef struct {
    char        method[HTTP_METHOD_MAX];
    char        path[HTTP_PATH_MAX];     /* decoded, without the query */
    char        query[HTTP_QUERY_MAX];   /* raw, without the '?'       */
    const char *body;                    /* NUL-terminated, or NULL    */
    size_t      body_len;
} HttpRequest;

typedef struct {
    int    status;
    char   content_type[80];
    char  *body;
    size_t body_len;
    bool   body_owned;                   /* free(body) after sending   */
    char   extra_headers[256];
} HttpResponse;

/* Copies `text` into the response. */
void http_respond_text(HttpResponse *res, int status, const char *ctype,
                       const char *text);

/* Hands `buf` (malloc'd, len bytes) to the response, which frees it. */
void http_respond_owned(HttpResponse *res, int status, const char *ctype,
                        char *buf, size_t len);

/* References a string literal or other storage that outlives the response. */
void http_respond_static(HttpResponse *res, int status, const char *ctype,
                         const char *text);

/* Convenience for an error body in JSON. */
void http_respond_json_error(HttpResponse *res, int status, const char *msg);

typedef void (*HttpHandler)(const HttpRequest *req, HttpResponse *res,
                            void *ctx);

/* Called once the socket is listening, before the first accept(), so the
 * caller can print the URL and open a browser. */
typedef void (*HttpReadyFn)(int port, void *ctx);

/* Binds 127.0.0.1:port and serves until the process is interrupted.
 * Returns false only when the socket could not be set up, with a reason in
 * err -- the common case being that the port is already in use. */
bool http_serve(int port, HttpHandler handler, void *ctx, HttpReadyFn on_ready,
                char *err, size_t errsz);

/* Percent-decoding, exposed because the static file router needs it. */
void http_url_decode(char *s);

#endif /* SCHEDULIFY_HTTP_H */
