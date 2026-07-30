/* Unit tests for jf_config_load's handling of the optional transcode profile
 * line — build and run with `make test`.
 *
 * The config file is positional, so anything matched by shape rather than by
 * line number has to be pinned down: the risk isn't a profile that fails to
 * parse, it's a malformed one being silently consumed as an API key (or an
 * API key being eaten as a profile). Both directions are covered below.
 *
 * Writes and reads ./jellyfin.conf in the current directory, so run it
 * somewhere disposable — the Makefile target uses a temp dir. */
#include <stdio.h>
#include <string.h>
#include "jellyfin.h"
static int fails = 0, checks = 0;
static void t(const char *label, const char *body,
              int ew, int eh, int erate, const char *ekey, const char *emode) {
    FILE *f = fopen("./jellyfin.conf", "w"); fputs(body, f); fclose(f);
    JfConfig c; jf_config_load(&c);
    checks++;
    int ok = c.profile_width == ew && c.profile_height == eh &&
             c.profile_bitrate == erate && !strcmp(c.api_key, ekey) &&
             !strcmp(c.tv_mode, emode);
    if (!ok) { fails++;
        printf("  FAIL %s\n    got %dx%d@%d key=\"%s\" mode=%s\n    want %dx%d@%d key=\"%s\" mode=%s\n",
               label, c.profile_width, c.profile_height, c.profile_bitrate, c.api_key, c.tv_mode,
               ew, eh, erate, ekey, emode);
    }
}

/* DEBUGLOG gets the same "recognised wherever it appears, never eaten as a
 * credential" treatment as PAL/NTSC — checked separately from t() above
 * since none of its existing callers care about debug_log. */
static void tlog(const char *label, const char *body,
                  int edebug, const char *ekey, const char *euser) {
    FILE *f = fopen("./jellyfin.conf", "w"); fputs(body, f); fclose(f);
    JfConfig c; jf_config_load(&c);
    checks++;
    int ok = c.debug_log == edebug && !strcmp(c.api_key, ekey) && !strcmp(c.username, euser);
    if (!ok) { fails++;
        printf("  FAIL %s\n    got debug_log=%d key=\"%s\" user=\"%s\"\n"
               "    want debug_log=%d key=\"%s\" user=\"%s\"\n",
               label, c.debug_log, c.api_key, c.username, edebug, ekey, euser);
    }
}
int main(void) {
    t("no profile -> defaults", "http://s:8096\nPAL\n",
      720,576,12000000,"","PAL");
    t("WxH only", "http://s:8096\n640x480\nNTSC\n",
      640,480,12000000,"","NTSC");
    t("WxH@rate", "http://s:8096\n720x480@12000000\nPAL\n",
      720,480,12000000,"","PAL");
    t("with api key + username", "http://s:8096\nkey123\nbob\n640x480@9000000\nPAL\n",
      640,480,9000000,"key123","PAL");
    t("profile before credentials", "http://s:8096\n640x480\nkey123\nbob\n",
      640,480,12000000,"key123","PAL");
    /* Out of range: rejected, defaults kept, and NOT read as an api key. */
    t("too large -> ignored", "http://s:8096\n9999x9999\nPAL\n",
      720,576,12000000,"","PAL");
    t("bad bitrate -> size kept, rate default", "http://s:8096\n640x480@5\nPAL\n",
      640,480,12000000,"","PAL");
    /* Trailing junk must not parse, and must fall through to the api_key slot. */
    t("trailing junk is a credential", "http://s:8096\n480x270junk\nbob\n",
      720,576,12000000,"480x270junk","PAL");

    tlog("no DEBUGLOG -> off", "http://s:8096\nPAL\n", 0, "", "");
    tlog("DEBUGLOG anywhere -> on", "http://s:8096\nDEBUGLOG\nkey123\nbob\nPAL\n",
         1, "key123", "bob");
    tlog("DEBUGLOG lowercase -> on", "http://s:8096\ndebuglog\n", 1, "", "");
    tlog("DEBUGLOG not read as a credential", "http://s:8096\nkey123\nbob\nDEBUGLOG\nPAL\n",
         1, "key123", "bob");

    printf("profile: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
