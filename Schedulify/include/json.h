/*
 * json.h -- Minimal JSON reader and writer, in C.
 *
 * The web dashboard talks to the simulator over JSON, and the rule for this
 * project is that nothing about scheduling moves out of C.  So the encoding
 * lives here too: webapp.c parses the browser's request with the reader and
 * serialises the finished Schedule with the writer.  The browser only draws
 * what it is given.
 *
 * Deliberately small: objects, arrays, strings, numbers, booleans and null,
 * which is everything the API uses.  No streaming, no comments, no trailing
 * commas -- a malformed body is rejected with a message naming the offset.
 */
#ifndef SCHEDULIFY_JSON_H
#define SCHEDULIFY_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define JSON_ERR_MAX   160
#define JSON_MAX_DEPTH 32

/* ------------------------------------------------------------- reading -- */

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/* Parses `text`.  On success *out owns the tree and must be released with
 * json_free().  On failure *out is NULL and err describes the problem. */
bool json_parse(const char *text, JsonValue **out, char *err, size_t errsz);
void json_free(JsonValue *v);

JsonType json_type(const JsonValue *v);

/* Object member lookup; NULL when v is not an object or the key is absent. */
const JsonValue *json_get(const JsonValue *v, const char *key);

/* Array access; json_len returns 0 for anything that is not an array. */
int json_len(const JsonValue *v);
const JsonValue *json_at(const JsonValue *v, int index);

/* Typed extraction.  json_as_int rejects a non-integral or out-of-range
 * number, so "burst": 2.5 is an error rather than a silent truncation. */
bool json_as_int(const JsonValue *v, int *out);
bool json_as_bool(const JsonValue *v, bool *out);
bool json_as_double(const JsonValue *v, double *out);
const char *json_as_string(const JsonValue *v);   /* NULL unless a string */

/* Member shorthands.  Each returns false when the key is missing or the
 * value has the wrong type. */
bool json_member_int(const JsonValue *obj, const char *key, int *out);
bool json_member_bool(const JsonValue *obj, const char *key, bool *out);
bool json_member_double(const JsonValue *obj, const char *key,
                        double *out);
bool json_member_str(const JsonValue *obj, const char *key, char *buf,
                     size_t bufsz);

/* ------------------------------------------------------------- writing -- */

/* A growable JSON text buffer that tracks commas and nesting for you, so
 * callers never hand-write punctuation. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
    int    depth;
    bool   has_item[JSON_MAX_DEPTH];
} Json;

void json_init(Json *j);
void json_dispose(Json *j);

/* The accumulated text (always NUL-terminated), or "" after an allocation
 * failure.  json_ok() reports whether everything was written. */
const char *json_text(const Json *j);
size_t      json_size(const Json *j);
bool        json_ok(const Json *j);

void json_obj_begin(Json *j);
void json_obj_end(Json *j);
void json_arr_begin(Json *j);
void json_arr_end(Json *j);

/* Writes a key; the next value call becomes its value. */
void json_key(Json *j, const char *key);

/* Values, for use inside an array or straight after json_key(). */
void json_str(Json *j, const char *s);
void json_int(Json *j, long v);
void json_double(Json *j, double v, int decimals);
void json_bool(Json *j, bool b);
void json_null(Json *j);

/* key + value in one call. */
void json_kv_str(Json *j, const char *key, const char *s);
void json_kv_int(Json *j, const char *key, long v);
void json_kv_double(Json *j, const char *key, double v, int decimals);
void json_kv_bool(Json *j, const char *key, bool b);

#endif /* SCHEDULIFY_JSON_H */
