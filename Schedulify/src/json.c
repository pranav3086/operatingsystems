/*
 * json.c -- Minimal JSON reader and writer.
 */
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================= reading == */

struct JsonValue {
    JsonType type;
    double   num;
    bool     boolean;
    char    *str;          /* JSON_STRING                                  */
    char   **keys;         /* JSON_OBJECT, parallel to items               */
    JsonValue **items;     /* JSON_ARRAY / JSON_OBJECT                     */
    int      count;
    int      cap;
};

typedef struct {
    const char *s;
    size_t      pos;
    int         depth;
    char       *err;
    size_t      errsz;
    bool        failed;
} Parser;

static void p_fail(Parser *p, const char *what)
{
    if (p->failed) return;
    p->failed = true;
    if (p->err != NULL && p->errsz > 0)
        snprintf(p->err, p->errsz, "%s at offset %lu", what,
                 (unsigned long)p->pos);
}

static JsonValue *jv_new(JsonType t)
{
    JsonValue *v = calloc(1, sizeof *v);
    if (v != NULL) v->type = t;
    return v;
}

void json_free(JsonValue *v)
{
    int i;
    if (v == NULL) return;
    for (i = 0; i < v->count; i++) {
        if (v->keys != NULL) free(v->keys[i]);
        json_free(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v->str);
    free(v);
}

static bool jv_reserve(JsonValue *v, bool with_keys)
{
    int cap;
    JsonValue **gi;

    if (v->count < v->cap) return true;
    cap = (v->cap > 0) ? v->cap * 2 : 8;

    gi = realloc(v->items, (size_t)cap * sizeof *gi);
    if (gi == NULL) return false;
    v->items = gi;

    if (with_keys) {
        char **gk = realloc(v->keys, (size_t)cap * sizeof *gk);
        if (gk == NULL) return false;
        v->keys = gk;
    }
    v->cap = cap;
    return true;
}

static void skip_ws(Parser *p)
{
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t'
           || p->s[p->pos] == '\n' || p->s[p->pos] == '\r') {
        p->pos++;
    }
}

static JsonValue *parse_value(Parser *p);

/* Reads a JSON string literal (the opening quote is already current). */
static char *parse_string_raw(Parser *p)
{
    size_t cap = 16, len = 0;
    char *out;

    if (p->s[p->pos] != '"') { p_fail(p, "expected a string"); return NULL; }
    p->pos++;

    out = malloc(cap);
    if (out == NULL) { p_fail(p, "out of memory"); return NULL; }

    for (;;) {
        unsigned char c = (unsigned char)p->s[p->pos];

        if (c == '\0') {
            p_fail(p, "unterminated string");
            free(out);
            return NULL;
        }
        if (c == '"') { p->pos++; break; }

        if (c == '\\') {
            char esc = p->s[p->pos + 1];
            p->pos += 2;
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                /* \uXXXX -- encoded as UTF-8.  Surrogate pairs are not
                 * needed by this API, so a lone surrogate is passed
                 * through as the replacement character. */
                unsigned code = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    char h = p->s[p->pos + (size_t)k];
                    unsigned d;
                    if (h >= '0' && h <= '9')      d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                    else { p_fail(p, "bad \\u escape"); free(out); return NULL; }
                    code = code * 16u + d;
                }
                p->pos += 4;
                if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFDu;

                /* grow enough for the longest encoding plus NUL */
                while (len + 4 >= cap) {
                    char *g;
                    cap *= 2;
                    g = realloc(out, cap);
                    if (g == NULL) { p_fail(p, "out of memory");
                                     free(out); return NULL; }
                    out = g;
                }
                if (code < 0x80u) {
                    out[len++] = (char)code;
                } else if (code < 0x800u) {
                    out[len++] = (char)(0xC0u | (code >> 6));
                    out[len++] = (char)(0x80u | (code & 0x3Fu));
                } else {
                    out[len++] = (char)(0xE0u | (code >> 12));
                    out[len++] = (char)(0x80u | ((code >> 6) & 0x3Fu));
                    out[len++] = (char)(0x80u | (code & 0x3Fu));
                }
                continue;
            }
            default:
                p_fail(p, "unknown escape");
                free(out);
                return NULL;
            }
        } else if (c < 0x20) {
            p_fail(p, "raw control character in string");
            free(out);
            return NULL;
        } else {
            p->pos++;
        }

        if (len + 1 >= cap) {
            char *g;
            cap *= 2;
            g = realloc(out, cap);
            if (g == NULL) { p_fail(p, "out of memory"); free(out);
                             return NULL; }
            out = g;
        }
        out[len++] = (char)c;
    }

    out[len] = '\0';
    return out;
}

static JsonValue *parse_object(Parser *p)
{
    JsonValue *v = jv_new(JSON_OBJECT);
    if (v == NULL) { p_fail(p, "out of memory"); return NULL; }

    p->pos++;                       /* '{' */
    skip_ws(p);
    if (p->s[p->pos] == '}') { p->pos++; return v; }

    for (;;) {
        char *key;
        JsonValue *val;

        skip_ws(p);
        key = parse_string_raw(p);
        if (key == NULL) { json_free(v); return NULL; }

        skip_ws(p);
        if (p->s[p->pos] != ':') {
            p_fail(p, "expected ':'");
            free(key);
            json_free(v);
            return NULL;
        }
        p->pos++;

        val = parse_value(p);
        if (val == NULL) { free(key); json_free(v); return NULL; }

        if (!jv_reserve(v, true)) {
            p_fail(p, "out of memory");
            free(key);
            json_free(val);
            json_free(v);
            return NULL;
        }
        v->keys[v->count] = key;
        v->items[v->count] = val;
        v->count++;

        skip_ws(p);
        if (p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->s[p->pos] == '}') { p->pos++; return v; }

        p_fail(p, "expected ',' or '}'");
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_array(Parser *p)
{
    JsonValue *v = jv_new(JSON_ARRAY);
    if (v == NULL) { p_fail(p, "out of memory"); return NULL; }

    p->pos++;                       /* '[' */
    skip_ws(p);
    if (p->s[p->pos] == ']') { p->pos++; return v; }

    for (;;) {
        JsonValue *item = parse_value(p);
        if (item == NULL) { json_free(v); return NULL; }

        if (!jv_reserve(v, false)) {
            p_fail(p, "out of memory");
            json_free(item);
            json_free(v);
            return NULL;
        }
        v->items[v->count++] = item;

        skip_ws(p);
        if (p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->s[p->pos] == ']') { p->pos++; return v; }

        p_fail(p, "expected ',' or ']'");
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_number(Parser *p)
{
    const char *start = p->s + p->pos;
    char *endp = NULL;
    double d;
    JsonValue *v;

    errno = 0;
    d = strtod(start, &endp);
    if (endp == start) { p_fail(p, "expected a number"); return NULL; }
    if (errno == ERANGE) { p_fail(p, "number out of range"); return NULL; }

    p->pos += (size_t)(endp - start);

    v = jv_new(JSON_NUMBER);
    if (v == NULL) { p_fail(p, "out of memory"); return NULL; }
    v->num = d;
    return v;
}

static JsonValue *parse_literal(Parser *p)
{
    if (strncmp(p->s + p->pos, "true", 4) == 0) {
        JsonValue *v = jv_new(JSON_BOOL);
        if (v == NULL) { p_fail(p, "out of memory"); return NULL; }
        v->boolean = true;
        p->pos += 4;
        return v;
    }
    if (strncmp(p->s + p->pos, "false", 5) == 0) {
        JsonValue *v = jv_new(JSON_BOOL);
        if (v == NULL) { p_fail(p, "out of memory"); return NULL; }
        v->boolean = false;
        p->pos += 5;
        return v;
    }
    if (strncmp(p->s + p->pos, "null", 4) == 0) {
        JsonValue *v = jv_new(JSON_NULL);
        if (v == NULL) { p_fail(p, "out of memory"); return NULL; }
        p->pos += 4;
        return v;
    }
    p_fail(p, "unexpected token");
    return NULL;
}

static JsonValue *parse_value(Parser *p)
{
    JsonValue *v;
    char c;

    if (p->failed) return NULL;
    if (p->depth >= JSON_MAX_DEPTH) {
        p_fail(p, "nesting too deep");
        return NULL;
    }

    skip_ws(p);
    c = p->s[p->pos];

    p->depth++;
    if (c == '{')                       v = parse_object(p);
    else if (c == '[')                  v = parse_array(p);
    else if (c == '"') {
        char *s = parse_string_raw(p);
        if (s == NULL) { p->depth--; return NULL; }
        v = jv_new(JSON_STRING);
        if (v == NULL) { free(s); p_fail(p, "out of memory"); }
        else           v->str = s;
    }
    else if (c == '-' || (c >= '0' && c <= '9')) v = parse_number(p);
    else                                v = parse_literal(p);
    p->depth--;

    return v;
}

bool json_parse(const char *text, JsonValue **out, char *err, size_t errsz)
{
    Parser p;
    JsonValue *root;

    *out = NULL;
    if (err != NULL && errsz > 0) err[0] = '\0';

    if (text == NULL) {
        if (err != NULL && errsz > 0) snprintf(err, errsz, "empty body");
        return false;
    }

    p.s = text;
    p.pos = 0;
    p.depth = 0;
    p.err = err;
    p.errsz = errsz;
    p.failed = false;

    root = parse_value(&p);
    if (root == NULL) return false;

    skip_ws(&p);
    if (p.s[p.pos] != '\0') {
        p_fail(&p, "trailing characters after the JSON value");
        json_free(root);
        return false;
    }

    *out = root;
    return true;
}

/* ---------------------------------------------------------- accessors -- */

JsonType json_type(const JsonValue *v)
{
    return (v != NULL) ? v->type : JSON_NULL;
}

const JsonValue *json_get(const JsonValue *v, const char *key)
{
    int i;
    if (v == NULL || v->type != JSON_OBJECT || key == NULL) return NULL;
    for (i = 0; i < v->count; i++) {
        if (strcmp(v->keys[i], key) == 0) return v->items[i];
    }
    return NULL;
}

int json_len(const JsonValue *v)
{
    if (v == NULL || v->type != JSON_ARRAY) return 0;
    return v->count;
}

const JsonValue *json_at(const JsonValue *v, int index)
{
    if (v == NULL || v->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= v->count) return NULL;
    return v->items[index];
}

bool json_as_int(const JsonValue *v, int *out)
{
    if (v == NULL || v->type != JSON_NUMBER) return false;
    if (v->num != floor(v->num)) return false;          /* 2.5 is not an int */
    if (v->num < (double)INT_MIN || v->num > (double)INT_MAX) return false;
    *out = (int)v->num;
    return true;
}

bool json_as_bool(const JsonValue *v, bool *out)
{
    if (v == NULL || v->type != JSON_BOOL) return false;
    *out = v->boolean;
    return true;
}

bool json_as_double(const JsonValue *v, double *out)
{
    if (v == NULL || v->type != JSON_NUMBER) return false;
    *out = v->num;
    return true;
}

const char *json_as_string(const JsonValue *v)
{
    if (v == NULL || v->type != JSON_STRING) return NULL;
    return v->str;
}

bool json_member_int(const JsonValue *obj, const char *key, int *out)
{
    return json_as_int(json_get(obj, key), out);
}

bool json_member_bool(const JsonValue *obj, const char *key, bool *out)
{
    return json_as_bool(json_get(obj, key), out);
}

bool json_member_double(const JsonValue *obj, const char *key, double *out)
{
    return json_as_double(json_get(obj, key), out);
}

bool json_member_str(const JsonValue *obj, const char *key, char *buf,
                     size_t bufsz)
{
    const char *s = json_as_string(json_get(obj, key));
    if (s == NULL || bufsz == 0) return false;
    if (strlen(s) >= bufsz) return false;      /* refuse to truncate */
    memcpy(buf, s, strlen(s) + 1);
    return true;
}

/* ============================================================= writing == */

void json_init(Json *j)
{
    memset(j, 0, sizeof *j);
    j->cap = 1024;
    j->buf = malloc(j->cap);
    if (j->buf == NULL) { j->oom = true; j->cap = 0; return; }
    j->buf[0] = '\0';
}

void json_dispose(Json *j)
{
    free(j->buf);
    j->buf = NULL;
    j->len = 0;
    j->cap = 0;
}

const char *json_text(const Json *j)
{
    return (j->buf != NULL) ? j->buf : "";
}

size_t json_size(const Json *j)
{
    return j->len;
}

bool json_ok(const Json *j)
{
    return !j->oom;
}

static bool jb_room(Json *j, size_t extra)
{
    size_t need;
    char *g;

    if (j->oom) return false;
    need = j->len + extra + 1;
    if (need <= j->cap) return true;

    while (j->cap < need) j->cap = (j->cap > 0) ? j->cap * 2 : 1024;
    g = realloc(j->buf, j->cap);
    if (g == NULL) { j->oom = true; return false; }
    j->buf = g;
    return true;
}

static void jb_putn(Json *j, const char *s, size_t n)
{
    if (!jb_room(j, n)) return;
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static void jb_put(Json *j, const char *s)
{
    jb_putn(j, s, strlen(s));
}

/* Emits a comma when this container already holds an item. */
static void jb_separate(Json *j)
{
    if (j->depth > 0 && j->depth <= JSON_MAX_DEPTH) {
        if (j->has_item[j->depth - 1]) jb_put(j, ",");
        j->has_item[j->depth - 1] = true;
    }
}

static void jb_push(Json *j)
{
    if (j->depth < JSON_MAX_DEPTH) j->has_item[j->depth] = false;
    j->depth++;
}

static void jb_pop(Json *j)
{
    if (j->depth > 0) j->depth--;
}

void json_obj_begin(Json *j) { jb_separate(j); jb_put(j, "{"); jb_push(j); }
void json_obj_end(Json *j)   { jb_pop(j); jb_put(j, "}"); }
void json_arr_begin(Json *j) { jb_separate(j); jb_put(j, "["); jb_push(j); }
void json_arr_end(Json *j)   { jb_pop(j); jb_put(j, "]"); }

static void jb_escaped(Json *j, const char *s)
{
    jb_put(j, "\"");
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  jb_put(j, "\\\""); break;
        case '\\': jb_put(j, "\\\\"); break;
        case '\n': jb_put(j, "\\n");  break;
        case '\r': jb_put(j, "\\r");  break;
        case '\t': jb_put(j, "\\t");  break;
        case '\b': jb_put(j, "\\b");  break;
        case '\f': jb_put(j, "\\f");  break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                jb_put(j, esc);
            } else {
                jb_putn(j, (const char *)&c, 1);
            }
            break;
        }
    }
    jb_put(j, "\"");
}

void json_key(Json *j, const char *key)
{
    jb_separate(j);
    jb_escaped(j, key);
    jb_put(j, ":");
    /* The value that follows must not emit its own comma. */
    if (j->depth > 0 && j->depth <= JSON_MAX_DEPTH)
        j->has_item[j->depth - 1] = false;
}

/* After json_key() the separator flag was cleared, so a value written here
 * sets it again -- which is exactly what the next key needs to see. */
static void jb_value_prefix(Json *j)
{
    jb_separate(j);
}

void json_str(Json *j, const char *s)
{
    jb_value_prefix(j);
    jb_escaped(j, s);
}

void json_int(Json *j, long v)
{
    char tmp[32];
    jb_value_prefix(j);
    snprintf(tmp, sizeof tmp, "%ld", v);
    jb_put(j, tmp);
}

void json_double(Json *j, double v, int decimals)
{
    char tmp[64];
    jb_value_prefix(j);
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    /* JSON has no NaN or Infinity; emit null so the client sees a gap
     * rather than invalid syntax. */
    if (v != v || v > 1e308 || v < -1e308) {
        jb_put(j, "null");
        return;
    }
    snprintf(tmp, sizeof tmp, "%.*f", decimals, v);
    jb_put(j, tmp);
}

void json_bool(Json *j, bool b)
{
    jb_value_prefix(j);
    jb_put(j, b ? "true" : "false");
}

void json_null(Json *j)
{
    jb_value_prefix(j);
    jb_put(j, "null");
}

void json_kv_str(Json *j, const char *key, const char *s)
{
    json_key(j, key);
    json_str(j, s);
}

void json_kv_int(Json *j, const char *key, long v)
{
    json_key(j, key);
    json_int(j, v);
}

void json_kv_double(Json *j, const char *key, double v, int decimals)
{
    json_key(j, key);
    json_double(j, v, decimals);
}

void json_kv_bool(Json *j, const char *key, bool b)
{
    json_key(j, key);
    json_bool(j, b);
}
