/*
 * serve.c -- Start the dashboard server, point a browser at it.
 */
#include "serve.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "ui.h"
#include "webapp.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#endif

/* Opens the dashboard in a browser.  Chrome is tried first because that is
 * what the dashboard is developed and checked against; if it is not
 * installed the system default handler gets the URL instead. */
static void open_in_browser(const char *url)
{
#if defined(_WIN32)
    HINSTANCE h = ShellExecuteA(NULL, "open", "chrome", url, NULL,
                                SW_SHOWNORMAL);
    if ((intptr_t)h <= 32) {
        h = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        if ((intptr_t)h <= 32)
            ui_warn("could not launch a browser automatically -- open %s "
                    "yourself", url);
    }
#elif defined(__APPLE__)
    {
        char cmd[640];
        snprintf(cmd, sizeof cmd,
                 "open -a 'Google Chrome' '%s' >/dev/null 2>&1 "
                 "|| open '%s' >/dev/null 2>&1 &", url, url);
        if (system(cmd) != 0)
            ui_warn("could not launch a browser -- open %s yourself", url);
    }
#else
    {
        char cmd[768];
        snprintf(cmd, sizeof cmd,
                 "(google-chrome '%s' || google-chrome-stable '%s' "
                 "|| chromium '%s' || chromium-browser '%s' "
                 "|| xdg-open '%s') >/dev/null 2>&1 &",
                 url, url, url, url, url);
        if (system(cmd) != 0)
            ui_warn("could not launch a browser -- open %s yourself", url);
    }
#endif
}

typedef struct {
    WebApp app;
    bool   open_browser;
} ServeCtx;

static void on_ready(int port, void *ctx)
{
    ServeCtx *s = (ServeCtx *)ctx;
    char url[64];

    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);

    ui_title("Schedulify dashboard");
    printf("  %slistening on%s  %s%s%s\n", ui_c(C_DIM), ui_c(C_RESET),
           ui_c(C_OK), url, ui_c(C_RESET));
    printf("  %sweb root%s      %s\n", ui_c(C_DIM), ui_c(C_RESET),
           s->app.web_root);
    printf("  %sbound to the loopback interface only -- not reachable from "
           "the network%s\n", ui_c(C_DIM), ui_c(C_RESET));
    printf("\n  %severy schedule and metric shown in the browser is computed "
           "by the C engine;%s\n", ui_c(C_DIM), ui_c(C_RESET));
    printf("  %sthe page only animates the timeline it is sent.%s\n",
           ui_c(C_DIM), ui_c(C_RESET));
    printf("\n  %spress Ctrl+C to stop the server%s\n\n",
           ui_c(C_LABEL), ui_c(C_RESET));
    fflush(stdout);

    if (s->open_browser) open_in_browser(url);
}

bool serve_dashboard(int port, const char *web_root, bool open_browser,
                     const char *argv0)
{
    ServeCtx ctx;
    char err[HTTP_ERR_MAX];

    ctx.open_browser = open_browser;

    if (!webapp_init(&ctx.app, web_root, argv0, err, sizeof err)) {
        ui_error("%s", err);
        return false;
    }
    if (!http_serve(port, webapp_handle, &ctx, on_ready, err, sizeof err)) {
        ui_error("%s", err);
        return false;
    }
    return true;   /* not reached: http_serve runs until interrupted */
}
