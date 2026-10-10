/*
 * serve.h -- Starts the local dashboard server and opens a browser at it.
 */
#ifndef SCHEDULIFY_SERVE_H
#define SCHEDULIFY_SERVE_H

#include <stdbool.h>

#define SERVE_DEFAULT_PORT 8752

/* Locates the web root, binds 127.0.0.1:port and serves until interrupted.
 * When open_browser is true, Chrome (falling back to the system default) is
 * pointed at the dashboard once the socket is listening.
 *
 * Returns false if the server could not start -- the web root was missing or
 * the port was busy -- after printing the reason.  Otherwise it does not
 * return: the caller is expected to stop the process with Ctrl+C. */
bool serve_dashboard(int port, const char *web_root, bool open_browser,
                     const char *argv0);

#endif /* SCHEDULIFY_SERVE_H */
