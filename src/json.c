#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Guards against stack exhaustion from a pathologically nested document.
 * Jellyfin's own responses nest maybe five deep; anything approaching this is
 * not a real response. */
#define JSON_MAX_DEPTH 64

typedef struct {
    JsonDoc *doc;
    char    *p;
    int      depth;
    int      failed;
} JsonParser;

/* ── node pool ───────────────────────────────────────────────────────────── */

/* Returns an index, never a pointer: the pool is realloc'd as it grows, so any
 * JsonNode* held across another node_alloc() would dangle. Everything below
 * therefore re-derives pointers from doc->nodes[idx] at the point of use. */
static int node_alloc(JsonDoc *doc)
{
    if (doc->count == doc->cap) {
        int ncap = doc->cap ? doc->cap * 2 : 128;
        JsonNode *grown = (JsonNode *)realloc(doc->nodes, (size_t)ncap * sizeof(JsonNode));
        if (!grown) return -1;
        doc->nodes = grown;
        doc->cap   = ncap;
    }
    int idx = doc->count++;
    JsonNode *n = &doc->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->first_child  = -1;
    n->next_sibling = -1;
    return idx;
}

/* ── scanning ────────────────────────────────────────────────────────────── */

static void skip_ws(JsonParser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int nib = hex_nibble(s[i]);
        if (nib < 0) return 0;
        v = (v << 4) | (unsigned)nib;
    }
    *out = v;
    return 1;
}

/* Appends one code point as UTF-8. Worst case 4 bytes out for a 12-byte
 * surrogate-pair escape in, so writing over the source can never outrun it. */
static void emit_utf8(char **out, unsigned cp)
{
    char *o = *out;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = o;
}

/* Unescapes a string literal over the top of itself and returns a pointer to
 * it, NUL-terminated. Safe in place because every escape sequence is longer
 * than what it decodes to, so the write cursor always trails the read cursor.
 * On return ps->p sits just past the closing quote. */
static char *parse_string(JsonParser *ps)
{
    if (*ps->p != '"') { ps->failed = 1; return NULL; }
    ps->p++;

    char *in  = ps->p;
    char *out = ps->p;
    char *start = out;

    while (*in && *in != '"') {
        if (*in != '\\') { *out++ = *in++; continue; }

        in++;
        switch (*in) {
        case '"':  *out++ = '"';  in++; break;
        case '\\': *out++ = '\\'; in++; break;
        case '/':  *out++ = '/';  in++; break;
        case 'b':  *out++ = '\b'; in++; break;
        case 'f':  *out++ = '\f'; in++; break;
        case 'n':  *out++ = '\n'; in++; break;
        case 'r':  *out++ = '\r'; in++; break;
        case 't':  *out++ = '\t'; in++; break;
        case 'u': {
            unsigned cp;
            if (!hex4(in + 1, &cp)) { ps->failed = 1; return NULL; }
            in += 5;
            /* A high surrogate is only half a code point — pair it with the
             * low surrogate that must follow. Jellyfin emits these for
             * anything outside the BMP (emoji in a title, most commonly). */
            if (cp >= 0xD800 && cp <= 0xDBFF && in[0] == '\\' && in[1] == 'u') {
                unsigned lo;
                if (hex4(in + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    in += 6;
                }
            }
            /* An unpaired surrogate isn't a valid code point; substituting
             * U+FFFD keeps the string well-formed rather than emitting bytes
             * no decoder will accept. */
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            emit_utf8(&out, cp);
            break;
        }
        default:
            ps->failed = 1;
            return NULL;
        }
    }

    if (*in != '"') { ps->failed = 1; return NULL; }
    ps->p = in + 1;
    *out  = '\0';   /* at or before the closing quote, which is already consumed */
    return start;
}

static int parse_value(JsonParser *ps);

static int parse_object(JsonParser *ps)
{
    int idx = node_alloc(ps->doc);
    if (idx < 0) { ps->failed = 1; return -1; }
    ps->doc->nodes[idx].type = JSON_OBJECT;

    ps->p++;   /* '{' */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return idx; }

    int last = -1;
    for (;;) {
        skip_ws(ps);
        char *key = parse_string(ps);
        if (!key) { ps->failed = 1; return -1; }

        skip_ws(ps);
        if (*ps->p != ':') { ps->failed = 1; return -1; }
        ps->p++;

        int child = parse_value(ps);
        if (child < 0) { ps->failed = 1; return -1; }

        ps->doc->nodes[child].key = key;
        if (last < 0) ps->doc->nodes[idx].first_child   = child;
        else          ps->doc->nodes[last].next_sibling = child;
        last = child;
        ps->doc->nodes[idx].child_count++;

        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return idx; }
        ps->failed = 1;
        return -1;
    }
}

static int parse_array(JsonParser *ps)
{
    int idx = node_alloc(ps->doc);
    if (idx < 0) { ps->failed = 1; return -1; }
    ps->doc->nodes[idx].type = JSON_ARRAY;

    ps->p++;   /* '[' */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return idx; }

    int last = -1;
    for (;;) {
        int child = parse_value(ps);
        if (child < 0) { ps->failed = 1; return -1; }

        if (last < 0) ps->doc->nodes[idx].first_child   = child;
        else          ps->doc->nodes[last].next_sibling = child;
        last = child;
        ps->doc->nodes[idx].child_count++;

        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return idx; }
        ps->failed = 1;
        return -1;
    }
}

static int parse_value(JsonParser *ps)
{
    if (ps->failed) return -1;
    if (++ps->depth > JSON_MAX_DEPTH) { ps->failed = 1; return -1; }

    skip_ws(ps);

    int idx = -1;
    switch (*ps->p) {
    case '{': idx = parse_object(ps); break;
    case '[': idx = parse_array(ps);  break;
    case '"': {
        char *s = parse_string(ps);
        if (!s) break;
        idx = node_alloc(ps->doc);
        if (idx < 0) { ps->failed = 1; break; }
        ps->doc->nodes[idx].type = JSON_STRING;
        ps->doc->nodes[idx].str  = s;
        break;
    }
    case 't':
        if (strncmp(ps->p, "true", 4) != 0) { ps->failed = 1; break; }
        ps->p += 4;
        idx = node_alloc(ps->doc);
        if (idx < 0) { ps->failed = 1; break; }
        ps->doc->nodes[idx].type    = JSON_BOOL;
        ps->doc->nodes[idx].boolean = 1;
        break;
    case 'f':
        if (strncmp(ps->p, "false", 5) != 0) { ps->failed = 1; break; }
        ps->p += 5;
        idx = node_alloc(ps->doc);
        if (idx < 0) { ps->failed = 1; break; }
        ps->doc->nodes[idx].type    = JSON_BOOL;
        ps->doc->nodes[idx].boolean = 0;
        break;
    case 'n':
        if (strncmp(ps->p, "null", 4) != 0) { ps->failed = 1; break; }
        ps->p += 4;
        idx = node_alloc(ps->doc);
        if (idx < 0) { ps->failed = 1; break; }
        ps->doc->nodes[idx].type = JSON_NULL;
        break;
    default: {
        if (*ps->p != '-' && (*ps->p < '0' || *ps->p > '9')) { ps->failed = 1; break; }
        char *end_d = NULL;
        double d = strtod(ps->p, &end_d);
        if (end_d == ps->p) { ps->failed = 1; break; }
        /* Parsed as an integer as well as a double: tick counts run to ~10^14
         * and callers want them exact, without depending on how much mantissa
         * a double happens to have left. */
        long long ll = strtoll(ps->p, NULL, 10);
        ps->p = end_d;
        idx = node_alloc(ps->doc);
        if (idx < 0) { ps->failed = 1; break; }
        ps->doc->nodes[idx].type = JSON_NUMBER;
        ps->doc->nodes[idx].num  = d;
        ps->doc->nodes[idx].i64  = (int64_t)ll;
        break;
    }
    }

    ps->depth--;
    return ps->failed ? -1 : idx;
}

int json_parse(JsonDoc *doc, char *text)
{
    memset(doc, 0, sizeof(*doc));
    if (!text) return 0;
    doc->text = text;

    JsonParser ps = { doc, text, 0, 0 };
    int root = parse_value(&ps);
    if (root < 0 || ps.failed) { json_free(doc); doc->text = text; return 0; }

    /* Reject trailing garbage. A response cut short by a too-small read
     * buffer fails here rather than yielding a plausible-looking partial
     * tree — silently short lists were exactly the old failure mode. */
    skip_ws(&ps);
    if (*ps.p != '\0') { json_free(doc); doc->text = text; return 0; }

    return 1;
}

void json_free(JsonDoc *doc)
{
    if (!doc) return;
    free(doc->nodes);
    doc->nodes = NULL;
    doc->count = doc->cap = 0;
    doc->text  = NULL;   /* borrowed — the caller owns it */
}

/* ── lookup ──────────────────────────────────────────────────────────────── */

const JsonNode *json_root(const JsonDoc *doc)
{
    if (!doc || doc->count == 0) return NULL;
    return &doc->nodes[0];
}

const JsonNode *json_child_first(const JsonDoc *doc, const JsonNode *parent)
{
    if (!doc || !parent || parent->first_child < 0) return NULL;
    return &doc->nodes[parent->first_child];
}

const JsonNode *json_child_next(const JsonDoc *doc, const JsonNode *node)
{
    if (!doc || !node || node->next_sibling < 0) return NULL;
    return &doc->nodes[node->next_sibling];
}

static const JsonNode *object_member(const JsonDoc *doc, const JsonNode *obj,
                                      const char *name, int name_len)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    JSON_FOREACH(doc, obj, child) {
        if (child->key &&
            strncmp(child->key, name, (size_t)name_len) == 0 &&
            child->key[name_len] == '\0')
            return child;
    }
    return NULL;
}

static const JsonNode *array_element(const JsonDoc *doc, const JsonNode *arr, int index)
{
    if (!arr || arr->type != JSON_ARRAY || index < 0) return NULL;
    JSON_FOREACH(doc, arr, child) {
        if (index-- == 0) return child;
    }
    return NULL;
}

const JsonNode *json_find(const JsonDoc *doc, const JsonNode *from, const char *path)
{
    if (!doc) return NULL;
    const JsonNode *cur = from ? from : json_root(doc);
    if (!cur || !path) return cur;

    const char *p = path;
    while (*p && cur) {
        if (*p == '.') { p++; continue; }

        if (*p == '[') {
            p++;
            int index = 0, digits = 0;
            while (*p >= '0' && *p <= '9') { index = index * 10 + (*p++ - '0'); digits++; }
            if (!digits || *p != ']') return NULL;
            p++;
            cur = array_element(doc, cur, index);
            continue;
        }

        const char *seg = p;
        while (*p && *p != '.' && *p != '[') p++;
        cur = object_member(doc, cur, seg, (int)(p - seg));
    }
    return cur;
}

/* ── typed reads ─────────────────────────────────────────────────────────── */

const char *json_as_str(const JsonNode *n, const char *fallback)
{
    return (n && n->type == JSON_STRING) ? n->str : fallback;
}

int64_t json_as_i64(const JsonNode *n, int64_t fallback)
{
    if (!n) return fallback;
    if (n->type == JSON_NUMBER) return n->i64;
    if (n->type == JSON_BOOL)   return n->boolean;
    return fallback;
}

double json_as_dbl(const JsonNode *n, double fallback)
{
    if (!n) return fallback;
    if (n->type == JSON_NUMBER) return n->num;
    if (n->type == JSON_BOOL)   return n->boolean;
    return fallback;
}

int json_as_bool(const JsonNode *n, int fallback)
{
    if (!n) return fallback;
    if (n->type == JSON_BOOL)   return n->boolean;
    /* Jellyfin is consistent about real booleans, but treating a number as
     * truthy costs nothing and avoids a surprising false for 1. */
    if (n->type == JSON_NUMBER) return n->i64 != 0;
    return fallback;
}

int json_copy_str(const JsonDoc *doc, const JsonNode *from, const char *path,
                   char *out, int outlen)
{
    if (!out || outlen <= 0) return 0;
    out[0] = '\0';

    const JsonNode *n = json_find(doc, from, path);
    const char *s = json_as_str(n, NULL);
    if (!s) return 0;

    strncpy(out, s, (size_t)outlen - 1);
    out[outlen - 1] = '\0';
    return 1;
}
