#include "jellyfin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#define JF_BUF_SIZE   (256 * 1024)
#define JF_ITEM_BUF   (JF_OVERVIEW_LEN + JF_NAME_LEN + 512)

/* Ids and image tags below (item_id, series_id, season_id, parent_id, the
 * ImageTags.Primary hash) come from server JSON and get embedded into
 * popen()/system() shell command lines. Real Jellyfin ids/tags are GUIDs or
 * hex strings, so a strict allowlist costs nothing against a real server but
 * closes off shell-metacharacter injection from a malicious/compromised one. */
static void jf_sanitize_id(const char *in, char *out, int outlen)
{
    int i = 0;
    for (; in && in[i] && i < outlen - 1; i++) {
        char c = in[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    out[i] = '\0';
}

/* ── tiny flat-key JSON extractors (same technique as MiSTerDVD's json_str,
 *    adapted with int64/bool variants; safe because Jellyfin's per-item
 *    field names — Name, Overview, ProductionYear, RunTimeTicks, Played,
 *    PlaybackPositionTicks, Primary — are unique enough within one item's
 *    JSON object that a flat strstr scan cannot cross into a sibling field
 *    with a colliding name). ────────────────────────────────────────────── */

static int json_str(const char *buf, const char *key, char *out, int maxlen)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        if (*p == '\\') {
            p++;
            if (*p == 'n' || *p == 'r') out[i++] = ' ';
            else if (*p) out[i++] = *p;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

static int json_int64(const char *buf, const char *key, int64_t *out)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = strtoll(p, NULL, 10);
    return 1;
}

static int json_bool(const char *buf, const char *key, int *out)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

/* Extracts the first quoted string from a "key":[ "a", "b", ... ] array
 * (used for BackdropImageTags — we only ever want the first backdrop). */
static int json_first_array_str(const char *buf, const char *key, char *out, int maxlen)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') return 0;   /* empty array or not a string array */
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* Returns pointer to the matching closing brace for the object starting at
 * obj_start (which must point at '{'), or NULL. Skips braces inside strings. */
static const char *json_find_obj_end(const char *obj_start)
{
    int depth = 0, in_str = 0;
    for (const char *p = obj_start; *p; p++) {
        if (in_str) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

/* Fills in the fields shared by a browse-list row and a full single-item
 * fetch. Does NOT touch cast — that's only ever populated by
 * jf_get_item_details() via parse_cast(), since it needs Fields=People
 * (an extra cost we don't want on every row of every browse list). */
static void parse_item_fields(const char *item_buf, JfItem *it)
{
    memset(it, 0, sizeof(*it));
    it->index_number = -1;

    json_str(item_buf, "Id",   it->id,   sizeof(it->id));
    json_str(item_buf, "Name", it->name, sizeof(it->name));
    json_str(item_buf, "Overview", it->overview, sizeof(it->overview));
    json_str(item_buf, "Primary", it->image_tag, sizeof(it->image_tag));

    strncpy(it->image_item_id,    it->id, sizeof(it->image_item_id) - 1);
    strncpy(it->backdrop_item_id, it->id, sizeof(it->backdrop_item_id) - 1);
    strncpy(it->logo_item_id,     it->id, sizeof(it->logo_item_id) - 1);

    /* A track with no embedded art of its own (ImageTags.Primary empty —
     * common, plenty of files just don't have one) still has its album's
     * cover available directly on its own DTO as AlbumId/
     * AlbumPrimaryImageTag — confirmed on a real server (a Daft Punk album
     * with no per-track embedded art: tracks showed no cover client-side
     * until this fallback, other albums whose files DO have embedded art
     * were unaffected). Same pattern as the Logo/Backdrop fallback below. */
    if (!it->image_tag[0]) {
        if (json_str(item_buf, "AlbumPrimaryImageTag", it->image_tag, sizeof(it->image_tag)))
            json_str(item_buf, "AlbumId", it->image_item_id, sizeof(it->image_item_id));
    }

    /* Episodes/seasons normally don't carry their own Logo/BackdropImageTags —
     * that art lives on the series, exposed via ParentLogoImageTag/
     * ParentBackdropImageTags (+ ParentLogoItemId/ParentBackdropItemId to say
     * which item to actually fetch it from). Confirmed on a real server: an
     * episode's own ImageTags has no "Logo" key and BackdropImageTags is
     * empty, which is why the info screen for any TV episode never showed a
     * backdrop/logo before this fallback — not specific to one show. */
    if (!json_str(item_buf, "Logo", it->logo_tag, sizeof(it->logo_tag))) {
        if (json_str(item_buf, "ParentLogoImageTag", it->logo_tag, sizeof(it->logo_tag)))
            json_str(item_buf, "ParentLogoItemId", it->logo_item_id, sizeof(it->logo_item_id));
    }
    if (!json_first_array_str(item_buf, "BackdropImageTags", it->backdrop_tag, sizeof(it->backdrop_tag))) {
        if (json_first_array_str(item_buf, "ParentBackdropImageTags", it->backdrop_tag, sizeof(it->backdrop_tag)))
            json_str(item_buf, "ParentBackdropItemId", it->backdrop_item_id, sizeof(it->backdrop_item_id));
    }

    char type_buf[24] = {0};
    json_str(item_buf, "Type", type_buf, sizeof(type_buf));
    if      (!strcmp(type_buf, "Series"))       it->type = JF_TYPE_SERIES;
    else if (!strcmp(type_buf, "Season"))        it->type = JF_TYPE_SEASON;
    else if (!strcmp(type_buf, "Episode"))       it->type = JF_TYPE_EPISODE;
    else if (!strcmp(type_buf, "Movie"))         it->type = JF_TYPE_MOVIE;
    else if (!strcmp(type_buf, "MusicArtist"))   it->type = JF_TYPE_ARTIST;
    else if (!strcmp(type_buf, "MusicAlbum"))    it->type = JF_TYPE_ALBUM;
    else if (!strcmp(type_buf, "Audio"))         it->type = JF_TYPE_TRACK;
    else if (!strcmp(type_buf, "CollectionFolder") ||
             !strcmp(type_buf, "Folder"))        it->type = JF_TYPE_FOLDER;
    else                                          it->type = JF_TYPE_OTHER;

    if (it->type == JF_TYPE_TRACK) {
        json_str(item_buf, "Album", it->album, sizeof(it->album));
        json_str(item_buf, "AlbumArtist", it->artist, sizeof(it->artist));
    }

    json_str(item_buf, "CollectionType", it->collection_type, sizeof(it->collection_type));

    int64_t year = 0;
    if (json_int64(item_buf, "ProductionYear", &year))
        snprintf(it->year, sizeof(it->year), "%lld", (long long)year);

    json_int64(item_buf, "RunTimeTicks", &it->runtime_ticks);

    int64_t child_count = 0;
    if (json_int64(item_buf, "ChildCount", &child_count))
        it->child_count = (int)child_count;

    int64_t idx_num;
    if (json_int64(item_buf, "IndexNumber", &idx_num))
        it->index_number = (int)idx_num;

    json_int64(item_buf, "PlaybackPositionTicks", &it->resume_ticks);
    json_bool(item_buf, "Played", &it->played);
}

/* Populates it->cast[] (Actor entries only, first JF_MAX_CAST of them) from
 * a "People":[ ... ] array anywhere in buf. */
static void parse_cast(const char *buf, JfItem *it)
{
    it->cast_count = 0;
    const char *p = strstr(buf, "\"People\":[");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;

    while (it->cast_count < JF_MAX_CAST) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '{') break;
        const char *end = json_find_obj_end(p);
        if (!end) break;

        int len = (int)(end - p + 1);
        char person_buf[768];
        if (len > (int)sizeof(person_buf) - 1) len = (int)sizeof(person_buf) - 1;
        memcpy(person_buf, p, len);
        person_buf[len] = '\0';

        char type_buf[24] = {0};
        json_str(person_buf, "Type", type_buf, sizeof(type_buf));
        if (!strcmp(type_buf, "Actor")) {
            JfPerson *person = &it->cast[it->cast_count];
            memset(person, 0, sizeof(*person));
            json_str(person_buf, "Id", person->id, sizeof(person->id));
            json_str(person_buf, "Name", person->name, sizeof(person->name));
            json_str(person_buf, "Role", person->role, sizeof(person->role));
            json_str(person_buf, "PrimaryImageTag", person->image_tag, sizeof(person->image_tag));
            it->cast_count++;
        }

        p = end + 1;
    }
}

/* Populates it->subs[] and it->source_* from a "MediaStreams":[ ... ] array
 * anywhere in buf (confirmed shape on a real server: Type is the string
 * "Subtitle"/"Video"/"Audio" — not the numeric enum some other Jellyfin API
 * responses use). Subtitle label prefers Language; external/undefined
 * tracks often have no Language, so DisplayTitle is the fallback. Only the
 * first Video stream's specs are kept (source_* is for display only, next
 * to the subtitle picker — what's actually streamed is JfStreamProfile). */
static void parse_media_streams(const char *buf, JfItem *it)
{
    it->sub_count = 0;
    it->source_video_codec[0] = '\0';
    it->source_width = it->source_height = 0;
    it->source_bitrate = 0;
    int got_video = 0;

    const char *p = strstr(buf, "\"MediaStreams\":[");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;

    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '{') break;
        const char *end = json_find_obj_end(p);
        if (!end) break;

        int len = (int)(end - p + 1);
        char stream_buf[768];
        if (len > (int)sizeof(stream_buf) - 1) len = (int)sizeof(stream_buf) - 1;
        memcpy(stream_buf, p, len);
        stream_buf[len] = '\0';

        char type_buf[24] = {0};
        json_str(stream_buf, "Type", type_buf, sizeof(type_buf));
        if (!strcmp(type_buf, "Subtitle") && it->sub_count < JF_MAX_SUBS) {
            JfSubtitle *sub = &it->subs[it->sub_count];
            memset(sub, 0, sizeof(*sub));
            int64_t idx = 0;
            json_int64(stream_buf, "Index", &idx);
            sub->index = (int)idx;
            if (!json_str(stream_buf, "Language", sub->label, sizeof(sub->label)) || !sub->label[0])
                json_str(stream_buf, "DisplayTitle", sub->label, sizeof(sub->label));
            json_str(stream_buf, "Codec", sub->codec, sizeof(sub->codec));
            it->sub_count++;
        } else if (!strcmp(type_buf, "Video") && !got_video) {
            got_video = 1;
            json_str(stream_buf, "Codec", it->source_video_codec, sizeof(it->source_video_codec));
            int64_t w = 0, h = 0, br = 0;
            json_int64(stream_buf, "Width", &w);
            json_int64(stream_buf, "Height", &h);
            json_int64(stream_buf, "BitRate", &br);
            it->source_width = (int)w;
            it->source_height = (int)h;
            it->source_bitrate = br;
        }

        p = end + 1;
    }
}

/* Parses a {"Items":[ ... ]} BaseItemDtoQueryResult buffer into out[]. */
static int parse_item_list(const char *buf, JfItem *out, int max)
{
    int count = 0;
    const char *p = strstr(buf, "\"Items\":[");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;

    while (count < max) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '{') break;
        const char *end = json_find_obj_end(p);
        if (!end) break;

        int len = (int)(end - p + 1);
        if (len > JF_ITEM_BUF - 1) len = JF_ITEM_BUF - 1;
        char item_buf[JF_ITEM_BUF];
        memcpy(item_buf, p, len);
        item_buf[len] = '\0';

        parse_item_fields(item_buf, &out[count]);

        count++;
        p = end + 1;
    }
    return count;
}

/* ── curl transport ──────────────────────────────────────────────────────── */

/* Runs `curl -H "Authorization: ..." <url>`, captures stdout into buf.
 * Returns 1 on success (non-empty response), 0 on failure. */
static int jf_get(const JfConfig *cfg, const char *path_and_query, char *buf, int buflen)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "curl -sfk --max-time 8 "
        "-H 'Authorization: MediaBrowser Token=\"%s\"' "
        "'%s%s' 2>/dev/null",
        cfg->api_key, cfg->server, path_and_query);

    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, (size_t)buflen - 1, f);
    pclose(f);
    buf[n] = '\0';
    return n > 0;
}

/* Fire-and-forget POST with a JSON body (Sessions/Playing family). Body is
 * written to a temp file to avoid quoting issues in the curl command line. */
static void jf_post_json(const JfConfig *cfg, const char *path, const char *json_body)
{
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/misterfin_post_%d.json", getpid());

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fputs(json_body, f);
    fclose(f);

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "curl -sfk --max-time 5 -X POST "
        "-H 'Authorization: MediaBrowser Token=\"%s\"' "
        "-H 'Content-Type: application/json' "
        "--data-binary @%s "
        "'%s%s' >/dev/null 2>&1",
        cfg->api_key, tmp_path, cfg->server, path);
    system(cmd);
    unlink(tmp_path);
}

/* ── config ───────────────────────────────────────────────────────────────── */

int jf_config_load(JfConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    const char *paths[] = {
        "/media/fat/misterfin/jellyfin.conf",
        "./jellyfin.conf",
        NULL
    };
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return 0;

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        char *dst;
        int dstlen;
        switch (line_no) {
            case 0: dst = cfg->server;   dstlen = (int)sizeof(cfg->server);   break;
            case 1: dst = cfg->api_key;  dstlen = (int)sizeof(cfg->api_key);  break;
            case 2: dst = cfg->username; dstlen = (int)sizeof(cfg->username); break;
            default: dst = cfg->tv_mode; dstlen = (int)sizeof(cfg->tv_mode); break;
        }
        strncpy(dst, line, dstlen - 1);
        line_no++;
        if (line_no >= 4) break;
    }
    fclose(f);

    /* strip a trailing slash from the server URL so path concatenation is clean */
    size_t slen = strlen(cfg->server);
    if (slen > 0 && cfg->server[slen - 1] == '/') cfg->server[slen - 1] = '\0';

    if (strcasecmp(cfg->tv_mode, "NTSC") != 0)
        strncpy(cfg->tv_mode, "PAL", sizeof(cfg->tv_mode) - 1);

    return (cfg->server[0] && cfg->api_key[0] && cfg->username[0]);
}

int jf_resolve_user_id(JfConfig *cfg)
{
    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, "/Users", buf, sizeof(buf))) return -1;

    const char *p = strchr(buf, '[');
    if (!p) return -1;
    p++;

    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '{') break;
        const char *end = json_find_obj_end(p);
        if (!end) break;

        int len = (int)(end - p + 1);
        char ubuf[512];
        if (len > (int)sizeof(ubuf) - 1) len = (int)sizeof(ubuf) - 1;
        memcpy(ubuf, p, len);
        ubuf[len] = '\0';

        char name[64] = {0};
        json_str(ubuf, "Name", name, sizeof(name));
        if (!strcasecmp(name, cfg->username)) {
            json_str(ubuf, "Id", cfg->user_id, sizeof(cfg->user_id));
            return cfg->user_id[0] != '\0';
        }
        p = end + 1;
    }
    return 0;
}

/* ── browsing ─────────────────────────────────────────────────────────────── */

int jf_list_views(const JfConfig *cfg, JfItem *out, int max)
{
    char path[256];
    snprintf(path, sizeof(path), "/UserViews?userId=%s", cfg->user_id);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;
    int n = parse_item_list(buf, out, max);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_FOLDER;
    return n;
}

int64_t jf_count_items(const JfConfig *cfg, const char *parent_id, const char *item_type)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    char path[384];
    if (item_type && item_type[0])
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true&IncludeItemTypes=%s&Limit=0",
            cfg->user_id, safe_parent, item_type);
    else
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true&Limit=0",
            cfg->user_id, safe_parent);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return -1;
    int64_t count = 0;
    return json_int64(buf, "TotalRecordCount", &count) ? count : -1;
}

int jf_list_items(const JfConfig *cfg, const char *parent_id, JfItem *out, int max)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    /* ChildCount here (unlike on a top-level library view — see
     * jf_count_items's comment) is exactly what it sounds like for a
     * MusicAlbum: its own track count, confirmed against a real server. */
    char path[512];
    snprintf(path, sizeof(path),
        "/Items?userId=%s&ParentId=%s&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Overview,ProductionYear,RunTimeTicks,ChildCount&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
        cfg->user_id, safe_parent, max);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;
    return parse_item_list(buf, out, max);
}

int jf_list_items_recursive(const JfConfig *cfg, const char *parent_id,
                             const char *item_type, JfItem *out, int max)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    char path[560];
    if (item_type && item_type[0])
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true&IncludeItemTypes=%s"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=Overview,ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, item_type, max);
    else
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=Overview,ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, max);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;
    return parse_item_list(buf, out, max);
}

int jf_get_item_details(const JfConfig *cfg, const char *item_id, JfItem *out)
{
    char safe_id[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));

    char path[384];
    snprintf(path, sizeof(path),
        "/Items/%s?userId=%s&Fields=Overview,ProductionYear,RunTimeTicks,People,MediaStreams"
        "&EnableUserData=true&EnableImageTypes=Primary,Logo,Backdrop",
        safe_id, cfg->user_id);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;

    parse_item_fields(buf, out);
    parse_cast(buf, out);
    parse_media_streams(buf, out);
    return 1;
}

int jf_list_seasons(const JfConfig *cfg, const char *series_id, JfItem *out, int max)
{
    char safe_series[JF_ID_LEN];
    jf_sanitize_id(series_id, safe_series, sizeof(safe_series));

    char path[256];
    snprintf(path, sizeof(path), "/Shows/%s/Seasons?userId=%s", safe_series, cfg->user_id);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;
    int n = parse_item_list(buf, out, max);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_SEASON;
    return n;
}

int jf_list_episodes(const JfConfig *cfg, const char *series_id, const char *season_id,
                      JfItem *out, int max)
{
    char safe_series[JF_ID_LEN], safe_season[JF_ID_LEN];
    jf_sanitize_id(series_id, safe_series, sizeof(safe_series));
    jf_sanitize_id(season_id, safe_season, sizeof(safe_season));

    char path[384];
    snprintf(path, sizeof(path),
        "/Shows/%s/Episodes?seasonId=%s&userId=%s"
        "&Fields=Overview,RunTimeTicks&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary",
        safe_series, safe_season, cfg->user_id);

    static char buf[JF_BUF_SIZE];
    if (!jf_get(cfg, path, buf, sizeof(buf))) return 0;
    int n = parse_item_list(buf, out, max);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_EPISODE;
    return n;
}

/* ── images ───────────────────────────────────────────────────────────────── */

/* image_type is a caller-supplied constant (e.g. "Primary", "Logo",
 * "Backdrop/0"), never server/JSON data, so it's used as-is (it may
 * legitimately contain '/'); item_id and tag DO come from server JSON and
 * are sanitized. max_width <= 0 requests the original, full-size image —
 * always pass a real cap: Jellyfin resizes server-side (confirmed against a
 * real server: a 1.5MB backdrop dropped to ~15KB at maxWidth=320), which
 * matters a lot given this device's decode is scalar C with no SIMD path. */
int jf_item_image_url(const JfConfig *cfg, const char *item_id, const char *image_type,
                       const char *tag, int max_width, char *out, int outlen)
{
    if (!tag[0]) return 0;
    char safe_id[JF_ID_LEN], safe_tag[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(tag, safe_tag, sizeof(safe_tag));
    if (max_width > 0)
        snprintf(out, outlen, "%s/Items/%s/Images/%s?tag=%s&maxWidth=%d&quality=80",
                  cfg->server, safe_id, image_type, safe_tag, max_width);
    else
        snprintf(out, outlen, "%s/Items/%s/Images/%s?tag=%s",
                  cfg->server, safe_id, image_type, safe_tag);
    return 1;
}

int jf_download_item_image(const JfConfig *cfg, const char *item_id, const char *image_type,
                            const char *tag, int max_width, const char *dest_path)
{
    char url[512];
    if (!jf_item_image_url(cfg, item_id, image_type, tag, max_width, url, sizeof(url))) return 0;

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "curl -sfLk --max-time 15 '%s' -o '%s' 2>/dev/null", url, dest_path);
    return system(cmd) == 0;
}

int jf_image_url(const JfConfig *cfg, const JfItem *item, char *out, int outlen)
{
    return jf_item_image_url(cfg, item->id, "Primary", item->image_tag, 200, out, outlen);
}

int jf_download_image(const JfConfig *cfg, const JfItem *item, const char *dest_path)
{
    return jf_download_item_image(cfg, item->id, "Primary", item->image_tag, 200, dest_path);
}

/* ── playback ─────────────────────────────────────────────────────────────── */

int jf_stream_url(const JfConfig *cfg, const char *item_id,
                   const JfStreamProfile *profile, int64_t start_ticks,
                   const char *play_session_id, int burn_in_sub_index,
                   char *out, int outlen)
{
    char safe_id[JF_ID_LEN];
    char safe_session[64];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    /* audioCodec=mp3 (not aac) is deliberate: most sources here are already
     * AAC, and requesting audioCodec=aac lets Jellyfin just stream-copy the
     * original audio untouched — confirmed on a real server, even with
     * audioChannels=2 and allowAudioStreamCopy=false, neither budged it off
     * "copy". A source's original AAC track is very often 5.1, which means
     * mplayer has to decode 6 channels and downmix to stereo itself — extra
     * CPU work on top of an already-marginal software decode. Asking for a
     * codec that can't just be a source-format match (mp3) forces a genuine
     * transcode to stereo (confirmed: ffmpeg cmd showed
     * "-codec:a:0 libmp3lame -ac 2"), so mplayer only ever decodes simple
     * stereo MP3.
     *
     * videoCodec=mpeg2video (not h264): confirmed Jellyfin supports it as a
     * transcode target. MPEG-2 decode is meaningfully lighter than H.264 on
     * this weak Cortex-A9 (no CABAC/CAVLC entropy overhead, no deblocking
     * filter, simpler motion compensation) — MiSTerDVD already proves this
     * exact chip decodes real MPEG-2 DVD content smoothly, which H.264 at a
     * comparable resolution did not manage without desync. container=ts
     * forces MPEG-TS muxing instead of Jellyfin's default .mov for this
     * codec — TS is the container our -demuxer lavf fix is already proven
     * against; an untested mov path isn't worth the risk.
     *
     * burn_in_sub_index — see jf_stream_url's header comment for why this
     * is only ever set for image-based subtitle tracks. */
    char sub_params[64] = "";
    if (burn_in_sub_index >= 0)
        snprintf(sub_params, sizeof(sub_params),
                 "&subtitleStreamIndex=%d&subtitleMethod=Encode", burn_in_sub_index);

    snprintf(out, outlen,
        "%s/Videos/%s/stream?static=false&videoCodec=mpeg2video&container=ts"
        "&audioCodec=mp3&audioChannels=2"
        "&maxWidth=%d&maxHeight=%d&videoBitRate=%d"
        "&startTimeTicks=%lld&playSessionId=%s%s&ApiKey=%s",
        cfg->server, safe_id,
        profile->max_width, profile->max_height, profile->video_bitrate,
        (long long)(start_ticks > 0 ? start_ticks : 0),
        safe_session, sub_params, cfg->api_key);
    return 1;
}

/* Codec strings per ffmpeg/Jellyfin's known text subtitle codecs — anything
 * not in this list is treated as image-based (safer default: an unknown
 * text codec would just fail jf_download_subtitle's fetch harmlessly via
 * the existing "previous subtitle stays loaded" fallback in main.c, whereas
 * treating an actual image codec as text would silently show nothing with
 * no fallback at all). */
int jf_subtitle_is_text(const char *codec)
{
    static const char *const text_codecs[] = {
        "subrip", "srt", "ass", "ssa", "vtt", "webvtt", "mov_text",
        "microdvd", "sami", "smi", "ttml", "stl", NULL
    };
    for (int i = 0; text_codecs[i]; i++)
        if (!strcasecmp(codec, text_codecs[i])) return 1;
    return 0;
}

int jf_audio_stream_url(const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, char *out, int outlen)
{
    char safe_id[JF_ID_LEN];
    char safe_session[64];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    snprintf(out, outlen,
        "%s/Audio/%s/stream?static=true&playSessionId=%s&ApiKey=%s",
        cfg->server, safe_id, safe_session, cfg->api_key);
    return 1;
}

int jf_download_subtitle(const JfConfig *cfg, const char *item_id,
                          const char *media_source_id, int sub_index,
                          const char *dest_path)
{
    char safe_id[JF_ID_LEN], safe_msid[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(media_source_id, safe_msid, sizeof(safe_msid));

    char url[384];
    snprintf(url, sizeof(url), "%s/Videos/%s/%s/Subtitles/%d/Stream.srt?ApiKey=%s",
              cfg->server, safe_id, safe_msid, sub_index, cfg->api_key);

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "curl -sfLk --max-time 15 '%s' -o '%s' 2>/dev/null", url, dest_path);
    return system(cmd) == 0;
}

void jf_make_play_session_id(char *out, int outlen)
{
    snprintf(out, outlen, "misterfin-%d-%ld", (int)getpid(), (long)time(NULL));
}

static void build_playstate_json(const char *item_id, const char *play_session_id,
                                  int64_t position_ticks, int is_paused,
                                  char *out, int outlen)
{
    char safe_id[JF_ID_LEN], safe_session[64];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    snprintf(out, outlen,
        "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%lld,\"IsPaused\":%s,\"PlayMethod\":\"Transcode\"}",
        safe_id, safe_session, (long long)position_ticks,
        is_paused ? "true" : "false");
}

void jf_report_start(const JfConfig *cfg, const char *item_id,
                      const char *play_session_id, int64_t position_ticks)
{
    char body[512];
    build_playstate_json(item_id, play_session_id, position_ticks, 0, body, sizeof(body));
    jf_post_json(cfg, "/Sessions/Playing", body);
}

/* Sessions/Playing/Progress and /Stopped (used above for jf_report_start)
 * need a real logged-in session to correlate against — confirmed on a real
 * server: POSTing to them with just an API key returns 200/204 but silently
 * does NOT persist PlaybackPositionTicks (resume always came back to the
 * same old position no matter where playback actually stopped). The direct
 * per-user item data endpoint below has no such requirement and is
 * confirmed working. */
static void report_user_data(const JfConfig *cfg, const char *item_id,
                              int64_t position_ticks, int played)
{
    char safe_id[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    char path[256];
    snprintf(path, sizeof(path), "/UserItems/%s/UserData?userId=%s", safe_id, cfg->user_id);

    char body[128];
    snprintf(body, sizeof(body), "{\"PlaybackPositionTicks\":%lld,\"Played\":%s}",
              (long long)position_ticks, played ? "true" : "false");
    jf_post_json(cfg, path, body);
}

void jf_report_progress(const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks, int paused)
{
    (void)play_session_id; (void)paused;
    report_user_data(cfg, item_id, position_ticks, 0);
}

void jf_report_stopped(const JfConfig *cfg, const char *item_id,
                        const char *play_session_id, int64_t position_ticks)
{
    (void)play_session_id;
    report_user_data(cfg, item_id, position_ticks, 0);
}
