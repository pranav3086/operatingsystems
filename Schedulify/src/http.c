/*
 * http.c -- The tiny local HTTP server.
 */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID   INVALID_SOCKET
#  define sock_close(s)  closesocket(s)
#  define sock_errno()   WSAGetLastError()
   typedef int socklen_arg_t;
   /* GCC's -fanalyzer models socket()/accept() as POSIX file descriptors
    * that fail with -1. A Winsock SOCKET is unsigned and fails with
    * INVALID_SOCKET, so the analyser cannot recognise our checks and
    * reports phantom fd leaks and "use without check" on every call. The
    * POSIX build below is analysed with these checkers fully enabled. */
#  if defined(__GNUC__)
#    pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#    pragma GCC diagnostic ignored "-Wanalyzer-fd-use-without-check"
#  endif
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_INVALID   (-1)
#  define sock_close(s)  close(s)
#  define sock_errno()   errno
   typedef socklen_t socklen_arg_t;
#endif

/* ---------------------------------------------------------- responses -- */

static void set_ctype(HttpResponse *res, const char *ctype)
{
    snprintf(res->content_type, sizeof res->content_type, "%s",
             (ctype != NULL) ? ctype : "text/plain; charset=utf-8");
}

void http_respond_text(HttpResponse *res, int status, const char *ctype,
                       const char *text)
{
    size_t n = (text != NULL) ? strlen(text) : 0;
    char *copy = malloc(n + 1);

    res->status = status;
    set_ctype(res, ctype);
    if (copy == NULL) {
        res->body = NULL;
        res->body_len = 0;
        res->body_owned = false;
        return;
    }
    if (n > 0) memcpy(copy, text, n);
    copy[n] = '\0';
    res->body = copy;
    res->body_len = n;
    res->body_owned = true;
}

void http_respond_owned(HttpResponse *res, int status, const char *ctype,
                        char *buf, size_t len)
{
    res->status = status;
    set_ctype(res, ctype);
    res->body = buf;
    res->body_len = len;
    res->body_owned = true;
}

void http_respond_static(HttpResponse *res, int status, const char *ctype,
                         const char *text)
{
    res->status = status;
    set_ctype(res, ctype);
    /* Cast away const: body_owned is false, so it is never written or
     * freed -- the send path only reads it. */
    res->body = (char *)(size_t)(const void *)text;
    res->body_len = (text != NULL) ? strlen(text) : 0;
    res->body_owned = false;
}

void http_respond_json_error(HttpResponse *res, int status, const char *msg)
{
    char buf[512];
    const char *p;
    size_t n = 0;

    /* Hand-escape, because this path must work even when the JSON writer
     * is what failed. */
    n += (size_t)snprintf(buf + n, sizeof buf - n, "{\"error\":\"");
    for (p = msg; p != NULL && *p != '\0' && n + 8 < sizeof buf; p++) {
        if (*p == '"' || *p == '\\') {
            buf[n++] = '\\';
            buf[n++] = *p;
        } else if ((unsigned char)*p < 0x20) {
            buf[n++] = ' ';
        } else {
            buf[n++] = *p;
        }
    }
    buf[n] = '\0';
    snprintf(buf + n, sizeof buf - n, "\"}");
    http_respond_text(res, status, "application/json; charset=utf-8", buf);
}

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

/* ------------------------------------------------------------- helpers -- */

void http_url_decode(char *s)
{
    char *w = s;
    const char *r = s;

    while (*r != '\0') {
        if (*r == '%' && r[1] != '\0' && r[2] != '\0') {
            int hi = -1, lo = -1;
            char a = r[1], b = r[2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)(hi * 16 + lo);
                r += 3;
                continue;
            }
        }
        if (*r == '+') { *w++ = ' '; r++; continue; }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Case-insensitive search for a header, returning its value. */
static const char *find_header(const char *head, const char *name,
                               size_t *out_len)
{
    size_t nlen = strlen(name);
    const char *line = head;

    while (line != NULL && *line != '\0') {
        const char *eol = strstr(line, "\r\n");
        size_t llen = (eol != NULL) ? (size_t)(eol - line) : strlen(line);

        if (llen > nlen && line[nlen] == ':') {
            size_t i;
            bool match = true;
            for (i = 0; i < nlen; i++) {
                char a = line[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) { match = false; break; }
            }
            if (match) {
                const char *v = line + nlen + 1;
                while (*v == ' ' || *v == '\t') v++;
                *out_len = (size_t)((line + llen) - v);
                return v;
            }
        }
        if (eol == NULL) break;
        line = eol + 2;
    }
    return NULL;
}

/* ------------------------------------------------------------ receiving */

/* Reads one complete request into *buf (malloc'd, NUL-terminated).
 * Returns the HTTP status to use: 200 on success, or an error status. */
static int read_request(sock_t c, char **buf, size_t *len)
{
    size_t cap = 8192, n = 0;
    char *b = malloc(cap);
    const char *head_end = NULL;
    size_t header_len = 0;
    size_t content_len = 0;
    bool have_len = false;

    *buf = NULL;
    *len = 0;
    if (b == NULL) return 500;

    for (;;) {
        int got;

        if (n + 1 >= cap) {
            char *g;
            if (cap >= HTTP_MAX_REQUEST) { free(b); return 413; }
            cap *= 2;
            g = realloc(b, cap);
            if (g == NULL) { free(b); return 500; }
            b = g;
        }

#if defined(_WIN32)
        got = recv(c, b + n, (int)(cap - n - 1), 0);
#else
        {
            ssize_t r = recv(c, b + n, cap - n - 1, 0);
            got = (r > 2147483647) ? 2147483647 : (int)r;
        }
#endif
        if (got <= 0) break;
        n += (size_t)got;
        b[n] = '\0';

        if (head_end == NULL) {
            head_end = strstr(b, "\r\n\r\n");
            if (head_end != NULL) {
                const char *v;
                size_t vlen;
                header_len = (size_t)(head_end - b) + 4;
                v = find_header(b, "Content-Length", &vlen);
                if (v != NULL) {
                    char num[32];
                    size_t copy = (vlen < sizeof num - 1) ? vlen
                                                          : sizeof num - 1;
                    memcpy(num, v, copy);
                    num[copy] = '\0';
                    content_len = (size_t)strtoul(num, NULL, 10);
                    if (content_len > HTTP_MAX_REQUEST) { free(b); return 413; }
                    have_len = true;
                }
            }
        }
        if (head_end != NULL) {
            if (!have_len) break;                        /* no body */
            if (n >= header_len + content_len) break;    /* body complete */
        }
    }

    if (head_end == NULL) { free(b); return 400; }

    *buf = b;
    *len = n;
    return 200;
}

static bool send_all(sock_t c, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int w;
#if defined(_WIN32)
        w = send(c, data + sent, (int)(len - sent), 0);
#else
        {
            ssize_t r = send(c, data + sent, len - sent, 0);
            w = (r > 2147483647) ? 2147483647 : (int)r;
        }
#endif
        if (w <= 0) return false;
        sent += (size_t)w;
    }
    return true;
}

static void send_response(sock_t c, const HttpResponse *res)
{
    char head[640];
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n"
                      "Cache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\n"
                      "Connection: close\r\n"
                      "%s"
                      "\r\n",
                      res->status, status_text(res->status),
                      res->content_type, (unsigned long)res->body_len,
                      res->extra_headers);

    if (hn <= 0) return;
    if (!send_all(c, head, (size_t)hn)) return;
    if (res->body != NULL && res->body_len > 0)
        (void)send_all(c, res->body, res->body_len);
}

/* ---------------------------------------------------------- the server -- */

bool http_serve(int port, HttpHandler handler, void *ctx, HttpReadyFn on_ready,
                char *err, size_t errsz)
{
    sock_t listener;
    struct sockaddr_in addr;
    int reuse = 1;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(err, errsz, "could not initialise Winsock");
        return false;
    }
#endif

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == SOCK_INVALID) {
        snprintf(err, errsz, "could not create a socket");
        return false;
    }

    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, (socklen_arg_t)sizeof reuse);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /* Loopback only: the dashboard is for this machine, not the network. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *)&addr,
             (socklen_arg_t)sizeof addr) != 0) {
        snprintf(err, errsz,
                 "could not bind 127.0.0.1:%d (error %d) -- the port is "
                 "probably already in use; try --port with another number",
                 port, sock_errno());
        sock_close(listener);
        return false;
    }
    if (listen(listener, 16) != 0) {
        snprintf(err, errsz, "could not listen on port %d", port);
        sock_close(listener);
        return false;
    }

    if (on_ready != NULL) on_ready(port, ctx);

    for (;;) {
        sock_t client = accept(listener, NULL, NULL);
        char *raw = NULL;
        size_t raw_len = 0;
        int status;
        HttpRequest req;
        HttpResponse res;

        if (client == SOCK_INVALID) continue;

        memset(&req, 0, sizeof req);
        memset(&res, 0, sizeof res);
        res.status = 200;

        status = read_request(client, &raw, &raw_len);
        if (status != 200) {
            http_respond_json_error(&res, status,
                                    (status == 413) ? "request too large"
                                                    : "malformed request");
            send_response(client, &res);
            if (res.body_owned) free(res.body);
            sock_close(client);
            continue;
        }

        /* Request line: METHOD SP TARGET SP HTTP/1.1 */
        {
            const char *p = raw;
            const char *sp1 = strchr(p, ' ');
            const char *sp2 = (sp1 != NULL) ? strchr(sp1 + 1, ' ') : NULL;
            const char *qmark;
            size_t mlen, tlen;

            if (sp1 == NULL || sp2 == NULL) {
                http_respond_json_error(&res, 400, "malformed request line");
                send_response(client, &res);
                if (res.body_owned) free(res.body);
                free(raw);
                sock_close(client);
                continue;
            }
            mlen = (size_t)(sp1 - p);
            if (mlen >= sizeof req.method) mlen = sizeof req.method - 1;
            memcpy(req.method, p, mlen);
            req.method[mlen] = '\0';

            tlen = (size_t)(sp2 - (sp1 + 1));
            {
                char target[HTTP_PATH_MAX + HTTP_QUERY_MAX];
                size_t plen;

                /* Reject rather than truncate: a shortened path could
                 * otherwise alias onto a different, valid route. */
                if (tlen >= sizeof target) {
                    http_respond_json_error(&res, 400, "request target too long");
                    send_response(client, &res);
                    if (res.body_owned) free(res.body);
                    free(raw);
                    sock_close(client);
                    continue;
                }
                memcpy(target, sp1 + 1, tlen);
                target[tlen] = '\0';

                qmark = strchr(target, '?');
                plen = (qmark != NULL) ? (size_t)(qmark - target) : tlen;
                if (plen >= sizeof req.path) {
                    http_respond_json_error(&res, 400, "request path too long");
                    send_response(client, &res);
                    if (res.body_owned) free(res.body);
                    free(raw);
                    sock_close(client);
                    continue;
                }
                memcpy(req.path, target, plen);
                req.path[plen] = '\0';
                if (qmark != NULL)
                    snprintf(req.query, sizeof req.query, "%s", qmark + 1);
            }
            http_url_decode(req.path);
        }

        /* Body */
        {
            const char *head_end = strstr(raw, "\r\n\r\n");
            if (head_end != NULL) {
                size_t off = (size_t)(head_end - raw) + 4;
                if (off <= raw_len) {
                    req.body = raw + off;
                    req.body_len = raw_len - off;
                }
            }
        }

        handler(&req, &res, ctx);
        send_response(client, &res);
        if (res.body_owned) free(res.body);
        free(raw);
        sock_close(client);
    }
}
