/* Unit tests for tunarr.c's config loading and pure URL-building — build and
 * run with `make test`. Field-extraction from live JSON responses isn't
 * covered here, matching jellyfin.c's own test coverage (test_config.c only
 * exercises jf_config_load; response parsing has no fixture-based tests
 * either) — see tests/test_json.c for the generic parser's own coverage.
 *
 * Writes and reads ./tunarr.conf in the current directory, so run it
 * somewhere disposable — the Makefile target uses a temp dir. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tunarr.h"

/* Internal to tunarr.c (not part of tunarr.h's public API), but not
 * `static` specifically so this file can exercise them directly — see
 * their definitions in tunarr.c for why. */
time_t xmltv_parse_time(const char *s);
void   xml_unescape_inplace(char *s);

static int fails = 0, checks = 0;

static void t(const char *label, const char *body, int eok, const char *eserver)
{
    FILE *f = fopen("./tunarr.conf", "w"); fputs(body, f); fclose(f);
    TunarrConfig c;
    int ok = tunarr_config_load(&c);
    checks++;
    if (ok != eok || strcmp(c.server, eserver) != 0) {
        fails++;
        printf("  FAIL %s\n    got ok=%d server=\"%s\"\n    want ok=%d server=\"%s\"\n",
               label, ok, c.server, eok, eserver);
    }
}

int main(void)
{
    t("plain server url", "http://192.168.2.20:8000\n", 1, "http://192.168.2.20:8000");
    t("trailing slash stripped", "http://192.168.2.20:8000/\n", 1, "http://192.168.2.20:8000");
    t("trailing whitespace/CRLF", "http://192.168.2.20:8000\r\n", 1, "http://192.168.2.20:8000");
    t("empty file", "", 0, "");
    t("blank first line", "\nhttp://192.168.2.20:8000\n", 0, "");

    /* Missing file entirely — remove it first. */
    remove("./tunarr.conf");
    { TunarrConfig c; checks++;
      int ok = tunarr_config_load(&c);
      if (ok != 0) { fails++; printf("  FAIL missing file should return 0, got %d\n", ok); } }

    /* tunarr_stream_url: pure string building, no network. */
    {
        TunarrConfig c; strncpy(c.server, "http://192.168.2.20:8000", sizeof(c.server) - 1);
        c.server[sizeof(c.server)-1] = '\0';
        char url[256];
        tunarr_stream_url(&c, "abc-123", url, sizeof(url));
        checks++;
        const char *want = "http://192.168.2.20:8000/stream/channels/abc-123.ts";
        if (strcmp(url, want) != 0) {
            fails++;
            printf("  FAIL stream url\n    got  \"%s\"\n    want \"%s\"\n", url, want);
        }
    }
    {
        /* Channel id with characters outside the allowlist gets sanitized —
         * confirms tunarr_sanitize_id is actually applied, not bypassed. */
        TunarrConfig c; strncpy(c.server, "http://s:8000", sizeof(c.server) - 1);
        c.server[sizeof(c.server)-1] = '\0';
        char url[256];
        tunarr_stream_url(&c, "abc/../etc", url, sizeof(url));
        checks++;
        const char *want = "http://s:8000/stream/channels/abc____etc.ts";
        if (strcmp(url, want) != 0) {
            fails++;
            printf("  FAIL sanitized stream url\n    got  \"%s\"\n    want \"%s\"\n", url, want);
        }
    }

    /* xmltv_parse_time: hand-rolled civil-date math, no timegm() dependency
     * (see its comment in tunarr.c) — worth pinning against known-correct
     * epoch values, unlike this file's usual policy of not fixture-testing
     * response parsing. Expected values cross-checked against Python's
     * datetime (UTC-aware) for the same wall-clock/offset inputs. */
    {
        struct { const char *label; const char *in; time_t want; } cases[] = {
            { "epoch",             "19700101000000 +0000", 0 },
            { "plain utc",         "20260812153159 +0000", 1786548719 },
            { "positive offset",   "20260812200159 +0530", 1786545119 },
            { "negative offset",   "20260812113159 -0400", 1786548719 },
            { "too short",         "2026081215",           0 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            checks++;
            time_t got = xmltv_parse_time(cases[i].in);
            if (got != cases[i].want) {
                fails++;
                printf("  FAIL xmltv_parse_time %s\n    got  %lld\n    want %lld\n",
                       cases[i].label, (long long)got, (long long)cases[i].want);
            }
        }
    }

    /* xml_unescape_inplace: the entities XMLTV titles actually carry. */
    {
        struct { const char *label; const char *in; const char *want; } cases[] = {
            { "no entities",  "Plain Title",                 "Plain Title" },
            { "amp",          "Rock &amp; Roll",              "Rock & Roll" },
            { "apos + #39",   "Don&apos;t &#39;Look Up&#39;", "Don't 'Look Up'" },
            { "lt/gt/quot",   "&lt;A &quot;B&quot; C&gt;",    "<A \"B\" C>" },
            { "unknown amp",  "Tom &amp Jerry",               "Tom &amp Jerry" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char buf[128];
            strncpy(buf, cases[i].in, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            xml_unescape_inplace(buf);
            checks++;
            if (strcmp(buf, cases[i].want) != 0) {
                fails++;
                printf("  FAIL xml_unescape_inplace %s\n    got  \"%s\"\n    want \"%s\"\n",
                       cases[i].label, buf, cases[i].want);
            }
        }
    }

    printf("tunarr: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
