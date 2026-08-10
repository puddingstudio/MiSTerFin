#include "tunarr.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Fills ch->program_title/start/stop from a parsed now_playing response.
 * Split out from tunarr_get_now_playing the same way parse_channels_json is,
 * for fixture-based unit testing. Returns 1 if a title was found. */
static int parse_now_playing_json(const JsonDoc *doc, TunarrChannel *ch)
{
    const JsonNode *root = json_root(doc);
    if (!root) return 0;

    const char *type = json_as_str(json_find(doc, root, "type"), "");
    char title[TUNARR_TITLE_LEN];
    title[0] = '\0';

    if (!strcmp(type, "content")) {
        json_copy_str(doc, root, "program.title", title, sizeof(title));
    } else if (!strcmp(type, "custom")) {
        json_copy_str(doc, root, "program.program.title", title, sizeof(title));
    } else if (!strcmp(type, "flex")) {
        json_copy_str(doc, root, "title", title, sizeof(title));
    } else if (!strcmp(type, "redirect")) {
        char redirected[TUNARR_NAME_LEN - 4];
        json_copy_str(doc, root, "channelName", redirected, sizeof(redirected));
        if (redirected[0]) snprintf(title, sizeof(title), "-> %s", redirected);
    }
    if (!title[0]) return 0;

    strncpy(ch->program_title, title, sizeof(ch->program_title) - 1);
    /* start/stop arrive as epoch milliseconds (confirmed against Tunarr's
     * OpenAPI spec); JfItem-style tick handling doesn't apply here, this is
     * plain Unix time. */
    ch->program_start = (time_t)(json_as_i64(json_find(doc, root, "start"), 0) / 1000);
    ch->program_stop  = (time_t)(json_as_i64(json_find(doc, root, "stop"), 0) / 1000);
    return 1;
}

int tunarr_get_now_playing(const TunarrConfig *cfg, TunarrChannel *ch)
{
    char safe_id[TUNARR_ID_LEN];
    tunarr_sanitize_id(ch->id, safe_id, sizeof(safe_id));

    char path[128];
    snprintf(path, sizeof(path), "/api/channels/%s/now_playing", safe_id);

    TunarrResponse r;
    if (!tunarr_fetch(cfg, path, &r)) return 0;
    int ok = parse_now_playing_json(&r.doc, ch);
    tunarr_response_free(&r);
    return ok;
}

void tunarr_stream_url(const TunarrConfig *cfg, const char *channel_id,
                        char *out, int outlen)
{
    char safe_id[TUNARR_ID_LEN];
    tunarr_sanitize_id(channel_id, safe_id, sizeof(safe_id));
    snprintf(out, outlen, "%s/stream/channels/%s.ts", cfg->server, safe_id);
}
