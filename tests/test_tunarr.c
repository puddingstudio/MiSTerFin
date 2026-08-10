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
#include "tunarr.h"

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

    printf("tunarr: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
