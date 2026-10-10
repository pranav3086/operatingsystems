/*
 * ui.c -- Terminal presentation layer.
 */
#include "ui.h"

#include "process.h"   /* SCHEDULIFY_VERSION */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define schedulify_isatty(fd) _isatty(fd)
#  define SCHEDULIFY_STDOUT_FD  1
#  define SCHEDULIFY_STDIN_FD   0
#else
#  include <unistd.h>
#  if defined(TIOCGWINSZ) || defined(__unix__) || defined(__APPLE__)
#    include <sys/ioctl.h>
#  endif
#  define schedulify_isatty(fd) isatty(fd)
#  define SCHEDULIFY_STDOUT_FD  STDOUT_FILENO
#  define SCHEDULIFY_STDIN_FD   STDIN_FILENO
#endif

#define FALLBACK_WIDTH 100
#define MIN_WIDTH      60
#define MAX_WIDTH      200

static bool g_color = false;
static bool g_unicode = false;
static int  g_width = FALLBACK_WIDTH;

static const BoxChars BOX_UNICODE = {
    "─", "│",
    "┌", "┐", "└", "┘",
    "┬", "┴",
    "├", "┤", "┼",
    "→", "•"
};

static const BoxChars BOX_ASCII = {
    "-", "|",
    "+", "+", "+", "+",
    "+", "+",
    "+", "+", "+",
    "->", "*"
};

static const char *const COLORS[C_COUNT] = {
    "\033[0m",       /* C_RESET   */
    "\033[1m",       /* C_BOLD    */
    "\033[2m",       /* C_DIM     */
    "\033[1;36m",    /* C_TITLE   */
    "\033[1;97m",    /* C_HEADING */
    "\033[33m",      /* C_LABEL   */
    "\033[97m",      /* C_VALUE   */
    "\033[32m",      /* C_OK      */
    "\033[33m",      /* C_WARN    */
    "\033[31m",      /* C_ERR     */
    "\033[90m",      /* C_IDLE    */
    "\033[35m"       /* C_ACCENT  */
};

/* ------------------------------------------------------------ capability */

static int detect_width(void)
{
    const char *env = getenv("COLUMNS");
    if (env != NULL && env[0] != '\0') {
        int v = atoi(env);
        if (v >= MIN_WIDTH) return (v > MAX_WIDTH) ? MAX_WIDTH : v;
    }
#if defined(_WIN32)
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
            int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            if (w >= MIN_WIDTH) return (w > MAX_WIDTH) ? MAX_WIDTH : w;
            if (w > 0) return MIN_WIDTH;
        }
    }
#elif defined(TIOCGWINSZ)
    {
        struct winsize ws;
        if (ioctl(SCHEDULIFY_STDOUT_FD, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            int w = (int)ws.ws_col;
            if (w >= MIN_WIDTH) return (w > MAX_WIDTH) ? MAX_WIDTH : w;
            return MIN_WIDTH;
        }
    }
#endif
    return FALLBACK_WIDTH;
}

static bool env_set(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0';
}

void ui_init(void)
{
    bool tty = schedulify_isatty(SCHEDULIFY_STDOUT_FD) != 0;
    bool vt_ok = tty;
    bool utf8_ok = false;

#if defined(_WIN32)
    if (tty) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        vt_ok = false;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
#           ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#           define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#           endif
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
                vt_ok = true;
        }
    }
    /* CP 65001 lets the box-drawing glyphs through on Windows Terminal and
     * on modern conhost; if it fails we stay on the ASCII fallback. */
    utf8_ok = tty && SetConsoleOutputCP(CP_UTF8) != 0;
#else
    {
        const char *lc = getenv("LC_ALL");
        if (lc == NULL || lc[0] == '\0') lc = getenv("LC_CTYPE");
        if (lc == NULL || lc[0] == '\0') lc = getenv("LANG");
        utf8_ok = (lc != NULL) && (strstr(lc, "UTF-8") != NULL
                                   || strstr(lc, "utf8") != NULL
                                   || strstr(lc, "UTF8") != NULL
                                   || strstr(lc, "utf-8") != NULL);
        /* Terminals that announce themselves are virtually always UTF-8 now,
         * but we only assume it for a real tty. */
        if (!utf8_ok && tty && env_set("TERM")) utf8_ok = true;
    }
#endif

    g_color = vt_ok && !env_set("NO_COLOR") && !env_set("SCHEDULIFY_NO_COLOR");
    g_unicode = utf8_ok && !env_set("SCHEDULIFY_ASCII");
    g_width = detect_width();
}

void ui_set_color_enabled(bool on)   { g_color = on; }
void ui_set_unicode_enabled(bool on) { g_unicode = on; }
bool ui_color_enabled(void)          { return g_color; }
bool ui_unicode_enabled(void)        { return g_unicode; }
int  ui_term_width(void)             { return g_width; }

bool ui_stdin_is_tty(void)
{
    return schedulify_isatty(SCHEDULIFY_STDIN_FD) != 0;
}

void ui_set_term_width(int w)
{
    if (w < MIN_WIDTH) w = MIN_WIDTH;
    if (w > MAX_WIDTH) w = MAX_WIDTH;
    g_width = w;
}

const BoxChars *ui_box(void)
{
    return g_unicode ? &BOX_UNICODE : &BOX_ASCII;
}

const char *ui_c(UiColor c)
{
    if (!g_color) return "";
    if (c < 0 || c >= C_COUNT) return "";
    return COLORS[c];
}

int ui_display_width(const char *s)
{
    int n = 0;
    if (s == NULL) return 0;
    for (; *s != '\0'; s++) {
        /* Count only UTF-8 lead bytes: every glyph used here is width 1. */
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    }
    return n;
}

/* -------------------------------------------------------------- printing */

void ui_clear_screen(void)
{
    if (g_color) {
        fputs("\033[2J\033[H", stdout);
    } else {
        fputc('\n', stdout);
    }
    fflush(stdout);
}

void ui_repeat(const char *glyph, int n)
{
    int i;
    for (i = 0; i < n; i++) fputs(glyph, stdout);
}

void ui_banner(void)
{
    const BoxChars *b = ui_box();
    int w = g_width - 2;
    const char *name = "SCHEDULIFY";
    const char *tag = "CPU Scheduling Simulator  v" SCHEDULIFY_VERSION
                      "  --  written in C11";
    int pad;

    fputs(ui_c(C_TITLE), stdout);
    fputs(b->tl, stdout); ui_repeat(b->h, w); fputs(b->tr, stdout);
    fputc('\n', stdout);

    pad = w - (int)strlen(name) - 1;
    if (pad < 0) pad = 0;
    printf("%s %s%s%s%*s%s\n", b->v, ui_c(C_BOLD), name, ui_c(C_TITLE),
           pad, "", b->v);

    pad = w - (int)strlen(tag) - 1;
    if (pad < 0) pad = 0;
    printf("%s %s%s%s%*s%s\n", b->v, ui_c(C_DIM), tag, ui_c(C_TITLE),
           pad, "", b->v);

    fputs(b->bl, stdout); ui_repeat(b->h, w); fputs(b->br, stdout);
    printf("%s\n", ui_c(C_RESET));
}

void ui_title(const char *fmt, ...)
{
    char buf[512];
    const BoxChars *b = ui_box();
    va_list ap;
    int inner;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    inner = ui_display_width(buf) + 2;
    if (inner > g_width - 2) inner = g_width - 2;

    fputc('\n', stdout);
    fputs(ui_c(C_TITLE), stdout);
    fputs(b->tl, stdout); ui_repeat(b->h, inner); fputs(b->tr, stdout);
    fputc('\n', stdout);
    printf("%s %s%s%s %s\n", b->v, ui_c(C_HEADING), buf, ui_c(C_TITLE), b->v);
    fputs(b->bl, stdout); ui_repeat(b->h, inner); fputs(b->br, stdout);
    printf("%s\n", ui_c(C_RESET));
}

void ui_section(const char *fmt, ...)
{
    char buf[512];
    const BoxChars *b = ui_box();
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    printf("\n%s%s%s\n", ui_c(C_LABEL), buf, ui_c(C_RESET));
    fputs(ui_c(C_DIM), stdout);
    ui_repeat(b->h, ui_display_width(buf));
    printf("%s\n", ui_c(C_RESET));
}

void ui_rule(void)
{
    const BoxChars *b = ui_box();
    fputs(ui_c(C_DIM), stdout);
    ui_repeat(b->h, g_width);
    printf("%s\n", ui_c(C_RESET));
}

static void indent_by(int n)
{
    int i;
    for (i = 0; i < n; i++) fputc(' ', stdout);
}

/* The one word-wrapper everything else is built on.  `indent` is the column
 * continuation lines start at; `col` is the column the cursor already sits
 * at, so a caller that has printed a label, a prefix or a heading can have
 * the rest of the text flow around it.  Prints neither a leading indent nor
 * a trailing newline.  Returns the column it finished on. */
static int wrap_body(const char *text, int indent, int col)
{
    int limit = g_width - 1;
    const char *p = text;

    if (limit < indent + 20) limit = indent + 20;

    while (*p != '\0') {
        const char *word;
        int wlen;

        while (*p == ' ') p++;
        if (*p == '\0') break;
        word = p;
        while (*p != '\0' && *p != ' ') p++;
        wlen = (int)(p - word);

        if (col > indent && col + 1 + wlen > limit) {
            fputc('\n', stdout);
            indent_by(indent);
            col = indent;
        }
        if (col > indent) { fputc(' ', stdout); col++; }
        fwrite(word, 1, (size_t)wlen, stdout);
        col += wlen;
    }
    return col;
}

void ui_wrap(const char *text, int indent)
{
    indent_by(indent);
    (void)wrap_body(text, indent, indent);
    fputc('\n', stdout);
}

void ui_wrap_dim(const char *text, int indent)
{
    fputs(ui_c(C_DIM), stdout);
    ui_wrap(text, indent);
    fputs(ui_c(C_RESET), stdout);
}

void ui_wrap_at(const char *text, int indent, int start_col)
{
    (void)wrap_body(text, indent, start_col);
}


/* Wraps a message to the output width, indenting continuation lines under
 * the prefix so "  !   some very long warning ..." stays readable. */
static void msg_wrapped(UiColor color, const char *prefix, const char *text)
{
    int indent = (int)strlen(prefix);

    fputs(ui_c(color), stdout);
    fputs(prefix, stdout);
    (void)wrap_body(text, indent, indent);
    printf("%s\n", ui_c(C_RESET));
}
#define DEFINE_MSG(fn, color, prefix)                      \
    void fn(const char *fmt, ...)                          \
    {                                                      \
        char buf[768];                                     \
        va_list ap;                                        \
        va_start(ap, fmt);                                 \
        vsnprintf(buf, sizeof buf, fmt, ap);               \
        va_end(ap);                                        \
        msg_wrapped((color), (prefix), buf);         \
    }

DEFINE_MSG(ui_info,    C_VALUE, "  ")
DEFINE_MSG(ui_success, C_OK,    "  OK  ")
DEFINE_MSG(ui_warn,    C_WARN,  "  !   ")
DEFINE_MSG(ui_error,   C_ERR,   "  x   ")
DEFINE_MSG(ui_note,    C_DIM,   "  note: ")

#undef DEFINE_MSG

void ui_prompt(const char *fmt, ...)
{
    char buf[768];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    printf("%s%s%s", ui_c(C_ACCENT), buf, ui_c(C_RESET));
    fflush(stdout);
}

void ui_menu_item(const char *key, const char *text)
{
    printf("  %s[%s]%s %s%s%s\n", ui_c(C_TITLE), key, ui_c(C_RESET),
           ui_c(C_VALUE), text, ui_c(C_RESET));
}

void ui_menu_item_dim(const char *key, const char *text)
{
    printf("  %s[%s] %s%s\n", ui_c(C_DIM), key, text, ui_c(C_RESET));
}

UiColor ui_proc_color(int index)
{
    static const UiColor palette[] = { C_OK, C_TITLE, C_LABEL, C_ACCENT,
                                       C_VALUE, C_WARN };
    if (index < 0) return C_IDLE;
    return palette[(size_t)index % (sizeof palette / sizeof palette[0])];
}
