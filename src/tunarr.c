#include "tunarr.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

/* Absolute path for the same reason jellyfin.c pins CURL_BIN — this process
 * runs as root on a MiSTer, so a planted "curl" earlier in PATH must not be
 * runnable in our place. */
#define TUNARR_CURL_BIN "/usr/bin/curl"

/* Hard ceiling on a captured response, same rationale as JF_MAX_RESPONSE:
 * legit channel-list/now-playing JSON is nowhere near this; it only trips on
 * a runaway/hostile stream. */
#define TUNARR_MAX_RESPONSE (8 * 1024 * 1024)

/* Ids here come from server JSON and are embedded in a URL passed to curl.
 * No shell sits between us and execv (see tunarr_curl_run), so this isn't
 * about injection — it's the same defensive allowlist jellyfin.c's
 * jf_sanitize_id applies, so a malformed/hostile id can't produce a URL path
 * curl interprets unexpectedly. Real Tunarr channel ids are UUIDs. */
static void tunarr_sanitize_id(const char *in, char *out, int outlen)
{
    int i = 0;
    for (; in && in[i] && i < outlen - 1; i++) {
        char c = in[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    out[i] = '\0';
}

/* Runs curl with an explicit argv and no shell, capturing stdout into a
 * heap buffer (caller frees). Same shape as jellyfin.c's jf_curl_run —
 * duplicated rather than shared because the two modules are deliberately
 * independent (see tunarr.h's header comment). */
static char *tunarr_curl_run(char *const argv[], int *exit_ok)
{
    if (exit_ok) *exit_ok = 0;

    int pfd[2];
    if (pipe(pfd) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return NULL; }

    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execv(TUNARR_CURL_BIN, argv);
        _exit(127);
    }

    close(pfd[1]);
    size_t cap = 64 * 1024, len = 0;
    char *buf = (char *)malloc(cap);
    if (buf) {
        for (;;) {
            if (len + 1 >= cap) {
                if (cap >= TUNARR_MAX_RESPONSE) { free(buf); buf = NULL; len = 0; break; }
                size_t ncap = cap * 2;
                char *grown = (char *)realloc(buf, ncap);
                if (!grown) { free(buf); buf = NULL; len = 0; break; }
                buf = grown;
                cap = ncap;
            }
            ssize_t got = read(pfd[0], buf + len, cap - len - 1);
            if (got <= 0) break;
            len += (size_t)got;
        }
    }
    if (!buf) { char sink[4096]; while (read(pfd[0], sink, sizeof(sink)) > 0) {} }
    close(pfd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (exit_ok) *exit_ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (!buf || len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* Fetch + parse in one step, mirroring jellyfin.c's JfResponse/jf_fetch. No
 * Authorization header — confirmed no Tunarr endpoint used here needs one. */
typedef struct {
    JsonDoc doc;
    char   *text;
} TunarrResponse;

static void tunarr_response_free(TunarrResponse *r)
{
    if (!r) return;
    json_free(&r->doc);
    free(r->text);
    r->text = NULL;
}

static int tunarr_fetch(const TunarrConfig *cfg, const char *path_and_query,
                         TunarrResponse *r)
{
    memset(r, 0, sizeof(*r));

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", cfg->server, path_and_query);

    const char *argv[8];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    argv[n++] = "--max-time";
    argv[n++] = "8";
    argv[n++] = url;
    argv[n]   = NULL;

    int ok = 0;
    r->text = tunarr_curl_run((char *const *)argv, &ok);
    if (!r->text) return 0;
    if (!json_parse(&r->doc, r->text)) { tunarr_response_free(r); return 0; }
    return 1;
}

/* Parsing split into its own function taking already-parsed JSON, so it's
 * unit-testable against fixture text without a live server or curl —
 * mirrors jellyfin.c's parse_item_list. */
static int parse_channels_json(const JsonDoc *doc, TunarrChannel *out, int max)
{
    const JsonNode *root = json_root(doc);
    if (!root) return 0;

    int count = 0;
    JSON_FOREACH(doc, root, ch) {
        if (count >= max) break;
        TunarrChannel *c = &out[count];
        memset(c, 0, sizeof(*c));
        json_copy_str(doc, ch, "id", c->id, sizeof(c->id));
        if (!c->id[0]) continue;   /* not a channel object — skip rather than emit a blank row */
        c->number = (int)json_as_i64(json_find(doc, ch, "number"), 0);
        json_copy_str(doc, ch, "name", c->name, sizeof(c->name));
        json_copy_str(doc, ch, "icon.path", c->icon_path, sizeof(c->icon_path));
        json_copy_str(doc, ch, "groupTitle", c->group_title, sizeof(c->group_title));
        count++;
    }
    return count;
}

static int channel_cmp_number(const void *a, const void *b)
{
    return ((const TunarrChannel *)a)->number - ((const TunarrChannel *)b)->number;
}

int tunarr_config_load(TunarrConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const char *paths[] = {
        "/media/fat/misterfin/tunarr.conf",
        "./tunarr.conf",
        NULL
    };
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return 0;

    char line[512];
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        strncpy(cfg->server, line, sizeof(cfg->server) - 1);
    }
    fclose(f);

    /* strip a trailing slash so path concatenation in tunarr_fetch is clean —
     * same rule jf_config_load applies to jellyfin.conf's server line. */
    size_t slen = strlen(cfg->server);
    if (slen > 0 && cfg->server[slen - 1] == '/') cfg->server[slen - 1] = '\0';

    return cfg->server[0] != '\0';
}

int tunarr_list_channels(const TunarrConfig *cfg, TunarrChannel *out, int max)
{
    TunarrResponse r;
    if (!tunarr_fetch(cfg, "/api/channels", &r)) return -1;
    int n = parse_channels_json(&r.doc, out, max);
    tunarr_response_free(&r);
    if (n > 1) qsort(out, (size_t)n, sizeof(out[0]), channel_cmp_number);
    return n;
}

/* ── XMLTV guide fetch ────────────────────────────────────────────────────
 * xmltv.xml is a flat, regular document — no need for a general XML parser,
 * just enough hand-rolled scanning to walk <programme start=".." stop=".."
 * channel="C<number>.<id>.tunarr.com"><title>..</title><icon src=".."/>
 * </programme> tags, mirroring json.c's "small, complete for this one
 * shape" approach rather than pulling in a real XML library. */

/* "20260812153159 +0000" -> Unix time (UTC). Hand-rolled rather than
 * struct-tm + timegm(): timegm isn't in the C standard (glibc extension of
 * varying availability), and this way the host build and the zig-cc
 * arm-linux-gnueabihf cross build can't disagree. The civil-to-days
 * conversion is Howard Hinnant's well-known algorithm (correct across the
 * whole Gregorian range, no library date table needed).
 *
 * Not declared in tunarr.h (it's an internal parsing detail, not part of
 * this module's public API) but deliberately not `static` either, so
 * tests/test_tunarr.c can forward-declare and exercise it directly — this
 * hand-rolled date math is exactly the kind of thing worth a fixture test,
 * unlike the JSON field-extraction this file otherwise leaves untested (see
 * this file's own top-of-file comment). */
time_t xmltv_parse_time(const char *s)
{
    if (!s || strlen(s) < 14) return 0;

    int off = 0;
    char buf[5];
#define XMLTV_DIGITS(n) (memcpy(buf, s + off, (size_t)(n)), buf[n] = '\0', off += (n), atoi(buf))
    int year  = XMLTV_DIGITS(4);
    int month = XMLTV_DIGITS(2);
    int day   = XMLTV_DIGITS(2);
    int hour  = XMLTV_DIGITS(2);
    int min   = XMLTV_DIGITS(2);
    int sec   = XMLTV_DIGITS(2);
#undef XMLTV_DIGITS

    int64_t y   = year - (month <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;

    time_t t = (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);

    /* " +HHMM" / " -HHMM" offset, if present — normalize to UTC. */
    const char *tz = s + 14;
    while (*tz == ' ') tz++;
    if ((*tz == '+' || *tz == '-') && strlen(tz + 1) >= 4) {
        int sign = (*tz == '-') ? 1 : -1;   /* "+0000" IS utc, so subtract to normalize */
        char hb[3] = { tz[1], tz[2], '\0' };
        char mb[3] = { tz[3], tz[4], '\0' };
        t += sign * (atoi(hb) * 3600 + atoi(mb) * 60);
    }
    return t;
}

/* The handful of XML entities Jellyfin/Tunarr titles actually produce —
 * unescaped in place, same in-place-shrink trick json.c uses for \u
 * escapes (the unescaped form is never longer than the source). Not
 * `static`, for the same reason as xmltv_parse_time above — see its
 * comment. */
void xml_unescape_inplace(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ) {
        if (*r == '&') {
            if      (!strncmp(r, "&amp;",  5)) { *w++ = '&';  r += 5; }
            else if (!strncmp(r, "&lt;",   4)) { *w++ = '<';  r += 4; }
            else if (!strncmp(r, "&gt;",   4)) { *w++ = '>';  r += 4; }
            else if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; }
            else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
            else if (!strncmp(r, "&#39;",  5)) { *w++ = '\''; r += 5; }
            else                                { *w++ = *r++; }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Reads `attr="value"` out of a tag's `<name ...>` span — bounded to the
 * tag itself (up to `tag_gt`) so it can't match an attribute belonging to
 * markup further down the document. */
static int xml_attr(const char *tag_start, const char *tag_gt, const char *attr,
                     char *out, int outlen)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=\"", attr);
    const char *p = strstr(tag_start, needle);
    if (!p || p >= tag_gt) return 0;
    p += strlen(needle);
    const char *q = strchr(p, '"');
    if (!q || q > tag_gt) return 0;
    int len = (int)(q - p);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* Text content between `after_gt` and the next `close_tag`, entity-unescaped
 * in place. Returns 0 if close_tag isn't found. */
static int xml_text(const char *after_gt, const char *close_tag, char *out, int outlen)
{
    const char *end = strstr(after_gt, close_tag);
    if (!end) return 0;
    int len = (int)(end - after_gt);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, after_gt, len);
    out[len] = '\0';
    xml_unescape_inplace(out);
    return 1;
}

static char *tunarr_fetch_raw(const TunarrConfig *cfg, const char *path, int *ok)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", cfg->server, path);

    const char *argv[8];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    argv[n++] = "--max-time";
    argv[n++] = "20";   /* xmltv.xml can be a multi-hundred-KB multi-day
                          * schedule dump, not a small API call — the 8s used
                          * elsewhere in this file is too tight for it. */
    argv[n++] = url;
    argv[n]   = NULL;
    return tunarr_curl_run((char *const *)argv, ok);
}

int tunarr_get_guide(const TunarrConfig *cfg, TunarrChannel *chans, int n)
{
    for (int i = 0; i < n; i++) chans[i].schedule_count = 0;
    if (n <= 0) return 0;

    int ok = 0;
    char *text = tunarr_fetch_raw(cfg, "/api/xmltv.xml", &ok);
    if (!text) return 0;

    /* xmltv.xml's <programme> entries for a given channel appear in
     * chronological order, and anything with stop <= now is already over
     * — so the first entry we see per channel with stop > now is either
     * whatever's airing right now (if start <= now) or, on a channel with
     * a gap in its lineup, the next thing that's going to air. Either way,
     * taking entries in the order encountered and appending them straight
     * into schedule[] (up to TUNARR_SCHEDULE_MAX) is exactly "now, then
     * what's next, then what's after that" — no separate now/next branches
     * needed. */
    time_t now = time(NULL);
    const char *p = text;
    while ((p = strstr(p, "<programme ")) != NULL) {
        const char *tag_gt = strchr(p, '>');
        if (!tag_gt) break;
        const char *body_end = strstr(tag_gt + 1, "</programme>");
        if (!body_end) break;

        char chan_attr[80], start_s[32], stop_s[32];
        if (xml_attr(p, tag_gt, "channel", chan_attr, sizeof(chan_attr)) &&
            xml_attr(p, tag_gt, "start", start_s, sizeof(start_s)) &&
            xml_attr(p, tag_gt, "stop", stop_s, sizeof(stop_s)) &&
            chan_attr[0] == 'C') {
            int num = atoi(chan_attr + 1);   /* "C112.148.tunarr.com" -> 112 */
            TunarrChannel *ch = NULL;
            for (int i = 0; i < n; i++) if (chans[i].number == num) { ch = &chans[i]; break; }

            if (ch && ch->schedule_count < TUNARR_SCHEDULE_MAX) {
                time_t stop = xmltv_parse_time(stop_s);
                if (stop > now) {
                    const char *title_tag = strstr(tag_gt + 1, "<title");
                    const char *title_gt  = (title_tag && title_tag < body_end) ?
                                             strchr(title_tag, '>') : NULL;
                    char title[TUNARR_TITLE_LEN];
                    if (title_gt && title_gt < body_end &&
                        xml_text(title_gt + 1, "</title>", title, sizeof(title))) {
                        TunarrScheduleItem *slot = &ch->schedule[ch->schedule_count++];
                        memset(slot, 0, sizeof(*slot));
                        strncpy(slot->title, title, sizeof(slot->title) - 1);
                        slot->start = xmltv_parse_time(start_s);
                        slot->stop  = stop;

                        char icon_attr[256];
                        const char *icon_tag = strstr(tag_gt + 1, "<icon ");
                        const char *icon_gt  = (icon_tag && icon_tag < body_end) ?
                                                strchr(icon_tag, '>') : NULL;
                        if (icon_gt && icon_gt < body_end &&
                            xml_attr(icon_tag, icon_gt, "src", icon_attr, sizeof(icon_attr)))
                            strncpy(slot->poster_url, icon_attr, sizeof(slot->poster_url) - 1);
                    }
                }
            }
        }
        p = body_end + strlen("</programme>");
    }

    free(text);
    return 1;
}

/* Case-insensitive substring search — used only to find the "location:"
 * header below. Not strcasestr(): that's a POSIX/BSD extension of uneven
 * availability, and this file already avoids libc extensions that might
 * behave differently on the zig-cc arm-linux-gnueabihf cross build (see
 * xmltv_parse_time's timegm() comment for the same rationale). */
static const char *find_ci(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return p;
    }
    return NULL;
}

int tunarr_download_image(const char *url, int max_width, const char *dest_path)
{
    char resized[900];
    const char *fetch_url = url;

    if (max_width > 0) {
        const char *head_argv[6] = { "curl", "-sI", "--max-time", "8", url, NULL };
        int hok = 0;
        char *headers = tunarr_curl_run((char *const *)head_argv, &hok);
        if (headers) {
            const char *loc = find_ci(headers, "location:");
            if (loc) {
                loc += strlen("location:");
                while (*loc == ' ') loc++;
                const char *end = loc;
                while (*end && *end != '\r' && *end != '\n') end++;
                int len = (int)(end - loc);
                if (len > 0 && len < (int)sizeof(resized) - 32) {
                    memcpy(resized, loc, (size_t)len);
                    resized[len] = '\0';
                    char sep = strchr(resized, '?') ? '&' : '?';
                    size_t used = (size_t)len;
                    snprintf(resized + used, sizeof(resized) - used, "%cmaxWidth=%d", sep, max_width);
                    fetch_url = resized;
                }
            }
            free(headers);
        }
    }

    const char *argv[8];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sfL";
    argv[n++] = "--max-time";
    argv[n++] = "15";
    argv[n++] = fetch_url;
    argv[n++] = "-o";
    argv[n++] = dest_path;
    argv[n]   = NULL;
    int ok = 0;
    tunarr_curl_run((char *const *)argv, &ok);
    return ok;
}

void tunarr_stream_url(const TunarrConfig *cfg, const char *channel_id,
                        char *out, int outlen)
{
    char safe_id[TUNARR_ID_LEN];
    tunarr_sanitize_id(channel_id, safe_id, sizeof(safe_id));
    snprintf(out, outlen, "%s/stream/channels/%s.ts", cfg->server, safe_id);
}
