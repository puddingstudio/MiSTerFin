/* Unit tests for src/json.c — build and run with `make test`.
 *
 * The parser sits under every server response the client reads, so the cases
 * that matter most here are the ones the old strstr-based scanner got wrong:
 * \uXXXX escapes (Jellyfin escapes apostrophes and all non-ASCII that way),
 * same-named keys nested inside sub-objects, and truncated input. */

#include <stdio.h>
#include <string.h>
#include "json.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                    \
    if (!(cond)) {                                               \
        failures++;                                              \
        printf("  FAIL %s:%d: ", __func__, __LINE__);            \
        printf(__VA_ARGS__);                                     \
        printf("\n");                                            \
    }                                                            \
} while (0)

#define CHECK_STR(doc, from, path, expect) do {                          \
    const char *got_ = json_as_str(json_find(doc, from, path), "(none)"); \
    CHECK(strcmp(got_, expect) == 0,                                     \
          "%s: expected \"%s\", got \"%s\"", path, expect, got_);        \
} while (0)

/* json_parse mutates its input, so every case needs its own writable copy. */
static char scratch[16384];
static char *dup_json(const char *src)
{
    strncpy(scratch, src, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';
    return scratch;
}

static void test_scalars(void)
{
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(
        "{\"s\":\"hi\",\"n\":42,\"neg\":-7,\"f\":3.5,\"t\":true,\"no\":false,\"nul\":null}")),
        "parse failed");

    CHECK_STR(&doc, NULL, "s", "hi");
    CHECK(json_as_i64(json_find(&doc, NULL, "n"), -1) == 42, "int");
    CHECK(json_as_i64(json_find(&doc, NULL, "neg"), 0) == -7, "negative int");
    CHECK(json_as_dbl(json_find(&doc, NULL, "f"), 0) == 3.5, "double");
    CHECK(json_as_bool(json_find(&doc, NULL, "t"), 0) == 1, "true");
    CHECK(json_as_bool(json_find(&doc, NULL, "no"), 1) == 0, "false");
    CHECK(json_find(&doc, NULL, "nul")->type == JSON_NULL, "null");

    /* A missing key and a wrong-typed key both fall back, so callers never
     * need a separate presence check. */
    CHECK(json_as_i64(json_find(&doc, NULL, "absent"), 99) == 99, "missing fallback");
    CHECK(json_as_i64(json_find(&doc, NULL, "s"), 99) == 99, "wrong-type fallback");
    json_free(&doc);
}

static void test_large_integers(void)
{
    /* RunTimeTicks are ~10^11..10^14. These must survive exactly, which is
     * why numbers carry an int64 alongside the double. */
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(
        "{\"RunTimeTicks\":72000000000,\"Pos\":13800000001}")), "parse failed");
    CHECK(json_as_i64(json_find(&doc, NULL, "RunTimeTicks"), 0) == 72000000000LL,
          "runtime ticks");
    CHECK(json_as_i64(json_find(&doc, NULL, "Pos"), 0) == 13800000001LL,
          "odd tick value survives exactly");
    json_free(&doc);
}

static void test_escapes(void)
{
    JsonDoc doc;
    /* Exactly what Jellyfin puts on the wire — confirmed against a real
     * server: the apostrophe in "Don't Look Up" arrives as '. */
    CHECK(json_parse(&doc, dup_json(
        "{\"a\":\"Don\\u0027t Look Up\","
        "\"b\":\"Am\\u00e9lie\","
        "\"c\":\"tab\\there\","
        "\"d\":\"quote\\\"inside\","
        "\"e\":\"back\\\\slash\","
        "\"f\":\"\\u0041\\u0042\"}")), "parse failed");

    CHECK_STR(&doc, NULL, "a", "Don't Look Up");
    CHECK_STR(&doc, NULL, "b", "Am\xC3\xA9lie");       /* U+00E9 as UTF-8 */
    CHECK_STR(&doc, NULL, "c", "tab\there");
    CHECK_STR(&doc, NULL, "d", "quote\"inside");
    CHECK_STR(&doc, NULL, "e", "back\\slash");
    CHECK_STR(&doc, NULL, "f", "AB");
    json_free(&doc);
}

static void test_surrogate_pairs(void)
{
    JsonDoc doc;
    /* U+1F600, the shape an emoji in a title arrives in. */
    CHECK(json_parse(&doc, dup_json("{\"e\":\"x\\ud83d\\ude00y\"}")), "parse failed");
    CHECK_STR(&doc, NULL, "e", "x\xF0\x9F\x98\x80y");
    json_free(&doc);   /* required before reusing the doc — see json_parse's contract */

    /* An unpaired high surrogate is not a code point — substituted rather
     * than emitted as invalid bytes. */
    CHECK(json_parse(&doc, dup_json("{\"e\":\"x\\ud83dy\"}")), "lone surrogate parse");
    CHECK_STR(&doc, NULL, "e", "x\xEF\xBF\xBDy");
    json_free(&doc);
}

static void test_nested_key_shadowing(void)
{
    /* The case that motivated replacing the scanner. "Type" appears both on
     * the item and on every entry of its People array; a flat search finds
     * whichever comes first in the byte stream, which is only correct by
     * accident of C# property declaration order upstream. */
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(
        "{\"People\":[{\"Name\":\"An Actor\",\"Type\":\"Actor\"}],"
        "\"Type\":\"Movie\",\"Name\":\"The Film\"}")), "parse failed");

    CHECK_STR(&doc, NULL, "Type", "Movie");
    CHECK_STR(&doc, NULL, "Name", "The Film");
    CHECK_STR(&doc, NULL, "People[0].Type", "Actor");
    CHECK_STR(&doc, NULL, "People[0].Name", "An Actor");
    json_free(&doc);
}

static void test_paths_and_arrays(void)
{
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(
        "{\"ImageTags\":{\"Primary\":\"abc123\",\"Logo\":\"def456\"},"
        "\"BackdropImageTags\":[\"bd0\",\"bd1\"],"
        "\"MediaStreams\":[{\"Index\":0,\"Type\":\"Video\"},"
                          "{\"Index\":1,\"Type\":\"Audio\",\"Language\":\"eng\"}]}")),
        "parse failed");

    CHECK_STR(&doc, NULL, "ImageTags.Primary", "abc123");
    CHECK_STR(&doc, NULL, "ImageTags.Logo", "def456");
    CHECK_STR(&doc, NULL, "BackdropImageTags[0]", "bd0");
    CHECK_STR(&doc, NULL, "BackdropImageTags[1]", "bd1");
    CHECK_STR(&doc, NULL, "MediaStreams[1].Language", "eng");
    CHECK(json_as_i64(json_find(&doc, NULL, "MediaStreams[1].Index"), -1) == 1, "nested index");
    CHECK(json_find(&doc, NULL, "BackdropImageTags[9]") == NULL, "out-of-range element");
    CHECK(json_find(&doc, NULL, "ImageTags.Missing") == NULL, "missing nested key");

    const JsonNode *streams = json_find(&doc, NULL, "MediaStreams");
    CHECK(streams && streams->child_count == 2, "array length");

    int seen = 0;
    JSON_FOREACH(&doc, streams, s) seen++;
    CHECK(seen == 2, "iteration count, got %d", seen);
    json_free(&doc);
}

static void test_relative_lookup(void)
{
    /* Iterating a list and reading fields relative to each element is the
     * core browse-list access pattern. */
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(
        "{\"Items\":[{\"Name\":\"One\",\"Id\":\"1\"},{\"Name\":\"Two\",\"Id\":\"2\"}],"
        "\"TotalRecordCount\":503}")), "parse failed");

    CHECK(json_as_i64(json_find(&doc, NULL, "TotalRecordCount"), 0) == 503, "total");

    const JsonNode *items = json_find(&doc, NULL, "Items");
    const char *names[2] = {0};
    int i = 0;
    JSON_FOREACH(&doc, items, it) {
        if (i < 2) names[i] = json_as_str(json_find(&doc, it, "Name"), "?");
        i++;
    }
    CHECK(i == 2, "two items");
    CHECK(names[0] && strcmp(names[0], "One") == 0, "first name");
    CHECK(names[1] && strcmp(names[1], "Two") == 0, "second name");
    json_free(&doc);
}

static void test_malformed_input(void)
{
    /* Truncation is the important one: the old scanner turned a response cut
     * short by a too-small buffer into a plausible short list. It must fail. */
    const char *bad[] = {
        "{\"Items\":[{\"Name\":\"trunc",      /* cut mid-string */
        "{\"Items\":[{\"Name\":\"x\"}",       /* cut mid-array */
        "{\"a\":}",
        "{\"a\" 1}",
        "{a:1}",
        "[1,2,",
        "{\"a\":01x}",
        "",
        "{\"a\":1}trailing",
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        JsonDoc doc;
        CHECK(json_parse(&doc, dup_json(bad[i])) == 0,
              "expected rejection of: %s", bad[i]);
        json_free(&doc);
    }

    /* Empty containers are valid, not malformed. */
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json("{\"Items\":[],\"TotalRecordCount\":0}")),
          "empty array should parse");
    CHECK(json_find(&doc, NULL, "Items")->child_count == 0, "empty array length");
    json_free(&doc);
}

static void test_deep_nesting_guard(void)
{
    /* Bounded recursion — a deliberately over-nested document must be
     * rejected rather than run the stack out. */
    char deep[512];
    int n = 0;
    for (; n < 200; n++) deep[n] = '[';
    deep[n] = '\0';

    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json(deep)) == 0, "deep nesting should be rejected");
    json_free(&doc);
}

static void test_copy_str(void)
{
    JsonDoc doc;
    CHECK(json_parse(&doc, dup_json("{\"Name\":\"A Fairly Long Title Here\"}")),
          "parse failed");

    char buf[8];
    CHECK(json_copy_str(&doc, NULL, "Name", buf, sizeof(buf)) == 1, "copy ok");
    CHECK(strcmp(buf, "A Fairl") == 0, "truncated safely, got \"%s\"", buf);
    CHECK(buf[sizeof(buf) - 1] == '\0', "NUL terminated");

    /* A miss must clear the buffer rather than leave stale content, so a
     * caller reusing one struct can't inherit a previous item's value. */
    strcpy(buf, "stale");
    CHECK(json_copy_str(&doc, NULL, "Absent", buf, sizeof(buf)) == 0, "miss returns 0");
    CHECK(buf[0] == '\0', "miss clears the buffer");
    json_free(&doc);
}

int main(void)
{
    test_scalars();
    test_large_integers();
    test_escapes();
    test_surrogate_pairs();
    test_nested_key_shadowing();
    test_paths_and_arrays();
    test_relative_lookup();
    test_malformed_input();
    test_deep_nesting_guard();
    test_copy_str();

    printf("json: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
