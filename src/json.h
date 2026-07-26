#pragma once
#include <stdint.h>

/* A small, complete JSON parser — enough to stop guessing at structure with
 * string searches.
 *
 * What it replaces was a set of flat strstr() scans for "key" anywhere in the
 * response. That worked only because Jellyfin happens to serialize its DTO
 * properties in an order where the field you wanted came before any nested
 * object that reused the same name — an invariant that lives in the order of
 * property declarations in a C# class upstream, and which nothing upstream
 * knows it is maintaining. It also could not handle \uXXXX escapes at all,
 * and Jellyfin escapes every non-ASCII character AND apostrophes that way, so
 * an ordinary title like "Don't Look Up" came through as "Donu0027t Look Up".
 *
 * Design notes:
 * - Parses in place. Strings are unescaped over the top of the source text
 *   (the unescaped form is never longer) and NUL-terminated, so a JsonNode's
 *   `str` points straight into the caller's buffer with no extra copies.
 *   That buffer must therefore stay alive and unmodified for the document's
 *   whole lifetime.
 * - Nodes live in one growable pool addressed by index rather than as
 *   individually allocated cells, so parsing a large response is a handful of
 *   reallocs rather than thousands of small mallocs.
 * - Every accessor tolerates a NULL node, so a lookup chain that misses can
 *   be read straight into a default without a separate presence check.
 */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct {
    JsonType    type;
    const char *key;          /* member name; NULL for array elements and the root */
    const char *str;          /* JSON_STRING: unescaped, NUL-terminated, UTF-8 */
    double      num;          /* JSON_NUMBER */
    int64_t     i64;          /* JSON_NUMBER, parsed as an integer separately so
                               * large tick counts don't go through a double */
    int         boolean;      /* JSON_BOOL */
    int         first_child;  /* node index, -1 if none */
    int         next_sibling; /* node index, -1 if none */
    int         child_count;  /* members of an object / elements of an array */
} JsonNode;

typedef struct {
    JsonNode *nodes;
    int       count;
    int       cap;
    char     *text;   /* borrowed and mutated in place — NOT owned or freed */
} JsonDoc;

/* Parses `text` in place. Returns 1 on success, 0 on malformed input (in
 * which case the doc is left empty and safe to json_free). `text` must remain
 * valid and unmodified until json_free — every string in the tree points into
 * it. Trailing content after the top-level value is rejected, so a response
 * truncated mid-object fails loudly instead of silently yielding a short list,
 * which is exactly the failure the old scanner turned into missing rows.
 *
 * `doc` must be fresh or already json_free'd: this takes it as uninitialized
 * and overwrites it, so parsing twice into the same doc without freeing in
 * between leaks the first tree. */
int  json_parse(JsonDoc *doc, char *text);
void json_free(JsonDoc *doc);

const JsonNode *json_root(const JsonDoc *doc);

/* Looks up a dotted path relative to `from` (NULL means the document root).
 * Supports object members and array subscripts:
 *     "Name"
 *     "ImageTags.Primary"
 *     "MediaStreams[0].Codec"
 * Returns NULL if any step is missing or the wrong type. */
const JsonNode *json_find(const JsonDoc *doc, const JsonNode *from, const char *path);

/* Child iteration, for arrays and objects alike. */
const JsonNode *json_child_first(const JsonDoc *doc, const JsonNode *parent);
const JsonNode *json_child_next (const JsonDoc *doc, const JsonNode *node);

#define JSON_FOREACH(doc, parent, it) \
    for (const JsonNode *it = json_child_first((doc), (parent)); \
         it != NULL; it = json_child_next((doc), it))

/* Typed reads. All accept node == NULL and return the fallback, so a missing
 * field and a present-but-wrong-type field behave identically. */
const char *json_as_str (const JsonNode *n, const char *fallback);
int64_t     json_as_i64 (const JsonNode *n, int64_t fallback);
double      json_as_dbl (const JsonNode *n, double fallback);
int         json_as_bool(const JsonNode *n, int fallback);

/* json_find + json_as_str + bounded copy. Returns 1 if the path resolved to a
 * string (and something was copied), 0 otherwise — in which case `out` is
 * left as an empty string rather than untouched. */
int json_copy_str(const JsonDoc *doc, const JsonNode *from, const char *path,
                   char *out, int outlen);
