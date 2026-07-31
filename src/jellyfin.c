#include "jellyfin.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

/* Absolute path so a planted "curl" earlier in PATH can't be run in our
 * place — this process is root on a MiSTer. /usr/bin/curl is where the
 * stock image ships it (also symlinked at /bin/curl); if it's ever
 * missing, execv fails and the request fails cleanly, same as any other
 * transport error. */
#define CURL_BIN "/usr/bin/curl"

/* Hard ceiling on a single captured response. Legit Jellyfin JSON for one
 * page of a large library is well under this; the cap only ever trips on a
 * runaway/hostile stream, which would otherwise grow the heap until the
 * kernel OOM-kills something. A capped request just fails and the caller
 * falls back the same way it does for any dropped connection. */
#define JF_MAX_RESPONSE (32 * 1024 * 1024)
/* Client/Device/Version identify us to Jellyfin's own Dashboard → Devices
 * list — without them the server had nothing to go on beyond the bare token
 * and showed up as a generic/guessed name instead of "MiSTerFin". DeviceId is
 * appended separately by jf_auth_header, since it's per-install rather than a
 * compile-time constant (see JfConfig.device_id for why that matters). */
#define JF_CLIENT_HEADERS \
    "Client=\"MiSTerFin\", Device=\"MiSTer FPGA\", Version=\"" APP_VERSION "\""

/* There is deliberately no lock in this file any more. Each fetch now reads
 * into its own heap buffer instead of a function-local "static char
 * buf[256K]", so two threads calling in at once (the home screen's
 * background cover prefetch does exactly that) no longer share anything
 * mutable. The mutex that used to serialize every request existed solely to
 * protect those static buffers — dropping it lets the prefetch thread
 * actually run in parallel with the main thread instead of taking turns. */

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

/* ── text for an ASCII-only display ──────────────────────────────────────── */

/* Normalises UTF-8 for the on-screen bitmap font. The font now covers ASCII
 * (0x00-0x7F) AND Latin-1 (0x00A0-0x00FF, see font8x8_ext_latin), so accented
 * Latin is kept as-is; only code points beyond that (Latin Extended-A, CJK,
 * Cyrillic, exotic punctuation) are folded to an ASCII approximation or a
 * single '?'.
 *
 * This became necessary the moment escapes were decoded properly. Jellyfin
 * escapes every non-ASCII character as \uXXXX, and the old scanner emitted
 * the escape's own letters verbatim — so "Amélie" reached the screen as
 * "Amu00e9lie". Decoding it correctly yields real UTF-8, which without this
 * would render as "Am??lie": more honest, but no more readable. Folding to
 * the base letter gives "Amelie", which is what someone actually wants to
 * see on a CRT.
 *
 * Accents are dropped rather than approximated with digraphs, except where a
 * digraph IS the conventional transliteration (ß→ss, Æ→AE). Anything with no
 * sensible ASCII form collapses to a single '?' per run, so a CJK title
 * reads as "?" rather than a wall of one '?' per byte. */
static void append_ascii(char *out, int outlen, int *pos, const char *s)
{
    while (*s && *pos < outlen - 1) out[(*pos)++] = *s++;
}

static const char *fold_codepoint(unsigned cp)
{
    /* Latin-1 Supplement, in code-point order from U+00C0. */
    static const char *const latin1[] = {
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
    };
    /* Latin Extended-A, U+0100..U+017F — base letters only; the pattern is
     * regular enough that a flat table is clearer than per-range rules. */
    static const char *const latin_a[] = {
        "A","a","A","a","A","a","C","c","C","c","C","c","C","c","D","d",
        "D","d","E","e","E","e","E","e","E","e","E","e","G","g","G","g",
        "G","g","G","g","H","h","H","h","I","i","I","i","I","i","I","i",
        "I","i","IJ","ij","J","j","K","k","k","L","l","L","l","L","l","L",
        "l","L","l","N","n","N","n","N","n","n","N","n","O","o","O","o",
        "O","o","OE","oe","R","r","R","r","R","r","S","s","S","s","S","s",
        "S","s","T","t","T","t","T","t","U","u","U","u","U","u","U","u",
        "U","u","U","u","W","w","Y","y","Y","Z","z","Z","z","Z","z","s"
    };

    if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) return latin_a[cp - 0x100];

    switch (cp) {
    case 0xA0: return " ";           /* non-breaking space */
    case 0xAB: return "\"";          /* « */
    case 0xBB: return "\"";          /* » */
    case 0x2010: case 0x2011:
    case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: return "-";
    case 0x2018: case 0x2019:
    case 0x201A: case 0x201B: return "'";
    case 0x201C: case 0x201D:
    case 0x201E: case 0x201F: return "\"";
    case 0x2026: return "...";
    case 0x2032: return "'";
    case 0x2033: return "\"";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    default: return NULL;
    }
}

/* Decodes one UTF-8 sequence at *s, advancing it. Returns the code point, or
 * 0xFFFD for an invalid sequence (consuming one byte so it always makes
 * progress). */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p;

    int extra;
    unsigned cp;
    if      (c < 0x80)         { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                       { *s += 1; return 0xFFFD; }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

void jf_text_to_display(const char *utf8, char *out, int outlen)
{
    if (!out || outlen <= 0) return;
    out[0] = '\0';
    if (!utf8) return;

    int pos = 0;
    int last_was_unknown = 0;
    const char *s = utf8;

    while (*s && pos < outlen - 1) {
        unsigned cp = utf8_next(&s);

        if (cp < 0x80) {
            /* Newlines inside an overview would break single-line layout;
             * the previous implementation flattened them to spaces too. */
            out[pos++] = (cp == '\n' || cp == '\r' || cp == '\t') ? ' ' : (char)cp;
            last_was_unknown = 0;
            continue;
        }

        if (cp >= 0xA0 && cp <= 0xFF) {
            /* The on-screen font now has Latin-1 glyphs (font8x8_ext_latin),
             * so keep these as real UTF-8 instead of folding to bare ASCII —
             * "Ressurreição" stays "Ressurreição", not "Ressurreicao". */
            if (pos < outlen - 2) {
                out[pos++] = (char)(0xC0 | (cp >> 6));
                out[pos++] = (char)(0x80 | (cp & 0x3F));
            }
            last_was_unknown = 0;
            continue;
        }

        const char *rep = fold_codepoint(cp);
        if (rep) {
            append_ascii(out, outlen, &pos, rep);
            last_was_unknown = 0;
        } else if (!last_was_unknown) {
            out[pos++] = '?';
            last_was_unknown = 1;   /* collapse a run into a single '?' */
        }
    }
    out[pos] = '\0';
}

/* json_copy_str + jf_text_to_display in one step — every string that ends up
 * on screen goes through this, so none of them can skip the fold. */
static int copy_display_str(const JsonDoc *doc, const JsonNode *from,
                             const char *path, char *out, int outlen)
{
    const JsonNode *n = json_find(doc, from, path);
    const char *s = json_as_str(n, NULL);
    if (!s) { if (outlen > 0) out[0] = '\0'; return 0; }
    jf_text_to_display(s, out, outlen);
    return 1;
}

/* Ids and image tags are opaque hex/GUID strings that never get displayed —
 * copy them verbatim, no folding. */
static int copy_raw_str(const JsonDoc *doc, const JsonNode *from,
                         const char *path, char *out, int outlen)
{
    return json_copy_str(doc, from, path, out, outlen);
}

/* ── item parsing ────────────────────────────────────────────────────────── */

/* Fills in the fields shared by a browse-list row and a full single-item
 * fetch. Does NOT touch cast — that's only ever populated by
 * jf_get_item_details() via parse_cast(), since it needs Fields=People
 * (an extra cost we don't want on every row of every browse list).
 *
 * `item` is the item's own object node, so every lookup below is scoped to
 * it. That scoping is the point: these same field names (Name, Type, Id)
 * also appear inside the nested People and MediaStreams arrays, and the
 * previous flat text search found whichever copy happened to come first in
 * the byte stream. It got the right answer only because Jellyfin's C# DTO
 * declares Type before People — an ordering no one upstream knows is load
 * bearing. */
static void parse_item_fields(const JsonDoc *doc, const JsonNode *item, JfItem *it)
{
    memset(it, 0, sizeof(*it));
    it->index_number = -1;

    copy_raw_str    (doc, item, "Id",       it->id,       sizeof(it->id));
    copy_display_str(doc, item, "Name",     it->name,     sizeof(it->name));
    copy_display_str(doc, item, "Overview", it->overview, sizeof(it->overview));
    copy_raw_str    (doc, item, "ImageTags.Primary", it->image_tag, sizeof(it->image_tag));
    it->community_rating = json_as_dbl(json_find(doc, item, "CommunityRating"), 0.0);

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
        if (copy_raw_str(doc, item, "AlbumPrimaryImageTag", it->image_tag, sizeof(it->image_tag)))
            copy_raw_str(doc, item, "AlbumId", it->image_item_id, sizeof(it->image_item_id));
    }

    /* Episodes/seasons normally don't carry their own Logo/BackdropImageTags —
     * that art lives on the series, exposed via ParentLogoImageTag/
     * ParentBackdropImageTags (+ ParentLogoItemId/ParentBackdropItemId to say
     * which item to actually fetch it from). Confirmed on a real server: an
     * episode's own ImageTags has no "Logo" key and BackdropImageTags is
     * empty, which is why the info screen for any TV episode never showed a
     * backdrop/logo before this fallback — not specific to one show. */
    if (!copy_raw_str(doc, item, "ImageTags.Logo", it->logo_tag, sizeof(it->logo_tag))) {
        if (copy_raw_str(doc, item, "ParentLogoImageTag", it->logo_tag, sizeof(it->logo_tag)))
            copy_raw_str(doc, item, "ParentLogoItemId", it->logo_item_id, sizeof(it->logo_item_id));
    }
    if (!copy_raw_str(doc, item, "BackdropImageTags[0]", it->backdrop_tag, sizeof(it->backdrop_tag))) {
        if (copy_raw_str(doc, item, "ParentBackdropImageTags[0]", it->backdrop_tag, sizeof(it->backdrop_tag)))
            copy_raw_str(doc, item, "ParentBackdropItemId", it->backdrop_item_id, sizeof(it->backdrop_item_id));
    }

    const char *type_buf = json_as_str(json_find(doc, item, "Type"), "");
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
        copy_display_str(doc, item, "Album",       it->album,  sizeof(it->album));
        copy_display_str(doc, item, "AlbumArtist", it->artist, sizeof(it->artist));
    }
    if (it->type == JF_TYPE_EPISODE)
        copy_display_str(doc, item, "SeriesName", it->series_name, sizeof(it->series_name));

    copy_raw_str(doc, item, "CollectionType", it->collection_type, sizeof(it->collection_type));

    int64_t year = json_as_i64(json_find(doc, item, "ProductionYear"), 0);
    if (year > 0) snprintf(it->year, sizeof(it->year), "%lld", (long long)year);

    it->runtime_ticks        = json_as_i64(json_find(doc, item, "RunTimeTicks"), 0);
    it->child_count          = (int)json_as_i64(json_find(doc, item, "ChildCount"), 0);
    it->recursive_item_count = (int)json_as_i64(json_find(doc, item, "RecursiveItemCount"), 0);
    it->index_number         = (int)json_as_i64(json_find(doc, item, "IndexNumber"), -1);
    it->parent_index_number  = (int)json_as_i64(json_find(doc, item, "ParentIndexNumber"), -1);

    /* Scoped to UserData rather than searched for loose: PlayedPercentage and
     * UnplayedItemCount live in the same object, and a bare "Played" search
     * was one rename away from matching the wrong one. */
    it->resume_ticks = json_as_i64 (json_find(doc, item, "UserData.PlaybackPositionTicks"), 0);
    it->played       = json_as_bool(json_find(doc, item, "UserData.Played"), 0);
}

/* Populates it->cast[] (Actor entries only, first JF_MAX_CAST of them) from
 * the item's People array. */
static void parse_cast(const JsonDoc *doc, const JsonNode *item, JfItem *it)
{
    it->cast_count = 0;
    const JsonNode *people = json_find(doc, item, "People");
    if (!people) return;

    JSON_FOREACH(doc, people, person_node) {
        if (it->cast_count >= JF_MAX_CAST) break;
        if (strcmp(json_as_str(json_find(doc, person_node, "Type"), ""), "Actor") != 0)
            continue;

        JfPerson *person = &it->cast[it->cast_count];
        memset(person, 0, sizeof(*person));
        copy_raw_str    (doc, person_node, "Id",              person->id,        sizeof(person->id));
        copy_display_str(doc, person_node, "Name",            person->name,      sizeof(person->name));
        copy_display_str(doc, person_node, "Role",            person->role,      sizeof(person->role));
        copy_raw_str    (doc, person_node, "PrimaryImageTag", person->image_tag, sizeof(person->image_tag));
        it->cast_count++;
    }
}

/* Populates it->subs[], it->audio[] and it->source_* from the item's
 * MediaStreams array (confirmed shape on a real server: Type is the string
 * "Subtitle"/"Video"/"Audio" — not the numeric enum some other Jellyfin API
 * responses use). Only the first Video stream's specs are kept (source_* is
 * for display only, next to the track pickers — what's actually streamed is
 * JfStreamProfile). */
static void parse_media_streams(const JsonDoc *doc, const JsonNode *item, JfItem *it)
{
    it->sub_count = 0;
    it->audio_count = 0;
    it->source_video_codec[0] = '\0';
    it->source_width = it->source_height = 0;
    it->source_bitrate = 0;
    it->source_aspect = 0.0;
    int got_video = 0;

    const JsonNode *streams = json_find(doc, item, "MediaStreams");
    if (!streams) return;

    JSON_FOREACH(doc, streams, stream) {
        const char *type_buf = json_as_str(json_find(doc, stream, "Type"), "");

        if (!strcmp(type_buf, "Subtitle") && it->sub_count < JF_MAX_SUBS) {
            JfSubtitle *sub = &it->subs[it->sub_count];
            memset(sub, 0, sizeof(*sub));
            sub->index = (int)json_as_i64(json_find(doc, stream, "Index"), 0);
            /* DisplayTitle first, Language only as a fallback. Language alone
             * is not enough to tell two tracks apart, and that's the common
             * case rather than an edge one: a release with English dialogue
             * subtitles AND an English signs/songs or forced track shows up
             * as two identical "eng" rows, with no way to know which is
             * which except by picking one and watching.
             *
             * The server already composes what's needed — DisplayTitle leads
             * with the track's own Title when it has one ("Signs & Songs"),
             * then appends whichever of Hearing Impaired / Default / Forced /
             * codec / External apply. Worth preferring even though it's
             * longer: if it overflows the label it's the trailing codec and
             * External tags that get cut, which are the least useful parts. */
            if (!copy_display_str(doc, stream, "DisplayTitle", sub->label, sizeof(sub->label)) ||
                !sub->label[0])
                copy_display_str(doc, stream, "Language", sub->label, sizeof(sub->label));
            copy_raw_str(doc, stream, "Codec", sub->codec, sizeof(sub->codec));
            sub->is_forced  = json_as_bool(json_find(doc, stream, "IsForced"), 0);
            sub->is_default = json_as_bool(json_find(doc, stream, "IsDefault"), 0);
            it->sub_count++;

        } else if (!strcmp(type_buf, "Audio") && it->audio_count < JF_MAX_AUDIO) {
            JfAudioTrack *aud = &it->audio[it->audio_count];
            memset(aud, 0, sizeof(*aud));
            aud->index    = (int)json_as_i64(json_find(doc, stream, "Index"), 0);
            aud->channels = (int)json_as_i64(json_find(doc, stream, "Channels"), 0);
            aud->is_default = json_as_bool(json_find(doc, stream, "IsDefault"), 0);
            copy_raw_str(doc, stream, "Codec", aud->codec, sizeof(aud->codec));
            /* DisplayTitle first here, unlike subtitles: the server composes
             * it as e.g. "English - Dolby Digital 5.1 - Default", which is
             * exactly what a track picker wants to show. Language alone
             * can't distinguish two English tracks (a commentary from the
             * main mix), which is the common case worth getting right. */
            if (!copy_display_str(doc, stream, "DisplayTitle", aud->label, sizeof(aud->label)) ||
                !aud->label[0])
                copy_display_str(doc, stream, "Language", aud->label, sizeof(aud->label));
            it->audio_count++;

        } else if (!strcmp(type_buf, "Video") && !got_video) {
            got_video = 1;
            copy_raw_str(doc, stream, "Codec", it->source_video_codec, sizeof(it->source_video_codec));
            it->source_width   = (int)json_as_i64(json_find(doc, stream, "Width"), 0);
            it->source_height  = (int)json_as_i64(json_find(doc, stream, "Height"), 0);
            it->source_bitrate = json_as_i64(json_find(doc, stream, "BitRate"), 0);

            const char *ar_buf = json_as_str(json_find(doc, stream, "AspectRatio"), "");
            int ar_w = 0, ar_h = 0;
            if (ar_buf[0] && sscanf(ar_buf, "%d:%d", &ar_w, &ar_h) == 2 && ar_h > 0)
                it->source_aspect = (double)ar_w / (double)ar_h;
        }
    }
}

/* Parses a {"Items":[ ... ]} BaseItemDtoQueryResult into out[]. */
static int parse_item_list(const JsonDoc *doc, JfItem *out, int max)
{
    const JsonNode *items = json_find(doc, NULL, "Items");
    if (!items) return 0;

    int count = 0;
    JSON_FOREACH(doc, items, item) {
        if (count >= max) break;
        parse_item_fields(doc, item, &out[count]);
        count++;
    }
    return count;
}

/* ── curl transport ──────────────────────────────────────────────────────── */

/* Builds the Authorization header.
 *
 * The Token part is omitted entirely when there isn't one, rather than sent
 * as Token="". That matters for exactly one call: QuickConnect/Initiate is
 * unauthenticated by definition (it's how you GET a credential), but it does
 * need the Client/Device/DeviceId fields, since those are what the server
 * shows the user when asking them to approve the request. */
static void jf_auth_header(const JfConfig *cfg, char *out, int outlen)
{
    /* Comma-space separated with Token last and omitted when absent — the
     * same shape jellyfin-apiclient-python builds (http.py's
     * _get_authenication_header), which is the reference for what real
     * clients send. */
    if (cfg->token[0])
        snprintf(out, outlen,
                 "Authorization: MediaBrowser " JF_CLIENT_HEADERS
                 ", DeviceId=\"%s\", Token=\"%s\"",
                 cfg->device_id, cfg->token);
    else
        snprintf(out, outlen,
                 "Authorization: MediaBrowser " JF_CLIENT_HEADERS ", DeviceId=\"%s\"",
                 cfg->device_id);
}

/* A fetched response and the parse tree over it. The tree's strings point
 * into `text`, so the two have to be freed together — jf_response_free does
 * both, and nothing should hold a JsonNode past that. */
typedef struct {
    JsonDoc doc;
    char   *text;
} JfResponse;

static void jf_response_free(JfResponse *r)
{
    if (!r) return;
    json_free(&r->doc);
    free(r->text);
    r->text = NULL;
}

/* Runs curl with an explicit argument vector and NO SHELL, optionally
 * capturing stdout into a NUL-terminated heap buffer (caller frees).
 * Returns the buffer when capture is set and something was read, else NULL;
 * *exit_ok (may be NULL) reports whether curl itself exited 0.
 *
 * The absence of a shell is the entire point. Every one of these arguments —
 * the Authorization header, the URL — carries data that ultimately came from
 * the server, and with popen()/system() that data was being parsed by /bin/sh.
 * A hostile server answering Quick Connect with
 *     {"AccessToken":"x' ; <command> ; '"}
 * got that command executed as root, on every launch thereafter, because the
 * token is saved to disk. Confirmed by reproduction, not theory. Note the
 * escape: Jellyfin encodes an apostrophe as ', so decoding escapes
 * correctly (which this client now does) is what turns a quoted string into
 * a quote-breaking one. execvp takes argv straight to the kernel — there is
 * no parser between us and exec, so no quoting to escape from.
 *
 * The capture path grows to fit rather than reading into a fixed buffer: the
 * old 256KB cap silently truncated large responses mid-JSON. */
static char *jf_curl_run(char *const argv[], int capture, int *exit_ok)
{
    if (exit_ok) *exit_ok = 0;

    int pfd[2];
    if (capture && pipe(pfd) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        if (capture) { close(pfd[0]); close(pfd[1]); }
        return NULL;
    }

    if (pid == 0) {
        if (capture) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        }
        /* curl's own diagnostics would otherwise land in the middle of the
         * framebuffer UI on the console. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (!capture) dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execv(CURL_BIN, argv);
        _exit(127);
    }

    char *buf = NULL;
    size_t len = 0;
    if (capture) {
        close(pfd[1]);
        size_t cap = 64 * 1024;
        buf = (char *)malloc(cap);
        if (buf) {
            for (;;) {
                if (len + 1 >= cap) {
                    if (cap >= JF_MAX_RESPONSE) { free(buf); buf = NULL; len = 0; break; }
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
        /* Drain anything left so curl never blocks on a full pipe while we're
         * waiting for it to exit. */
        if (!buf) { char sink[4096]; while (read(pfd[0], sink, sizeof(sink)) > 0) {} }
        close(pfd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (exit_ok) *exit_ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (!capture) { free(buf); return NULL; }
    if (!buf || len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* ── opt-in request log ──────────────────────────────────────────────────
 * See jf_log_init's doc comment in jellyfin.h for the privacy rules this is
 * built to guarantee structurally, not just by convention. */
static FILE *g_jf_log = NULL;

void jf_log_init(const JfConfig *cfg)
{
    if (!cfg->debug_log) return;   /* the only branch that ever touches the SD card */

    const char *paths[] = {
        "/media/fat/misterfin/debug.log",
        "./debug.log",
        NULL
    };
    for (int i = 0; paths[i] && !g_jf_log; i++)
        g_jf_log = fopen(paths[i], "w");   /* "w", not "a" — fresh per launch, see header */
    if (!g_jf_log) return;

    fprintf(g_jf_log, "MiSTerFin %s — tv_mode=%s profile=%dx%d@%d\n",
            APP_VERSION, cfg->tv_mode, cfg->profile_width, cfg->profile_height,
            cfg->profile_bitrate);
    fflush(g_jf_log);
}

void jf_log_close(void)
{
    if (g_jf_log) { fclose(g_jf_log); g_jf_log = NULL; }
}

/* method/path_and_query only — deliberately no cfg, no url, no auth header
 * in this function's signature at all, so there is nothing here capable of
 * writing a credential even by mistake. path_and_query is cut at the first
 * '?': every secret this app ever puts in a query string (the Quick Connect
 * secret, an ApiKey on a media URL) lands after that character, so the cut
 * removes it regardless of which endpoint this is. */
static void jf_log_request(const char *method, const char *path_and_query,
                            int ok, double elapsed_ms, long bytes)
{
    if (!g_jf_log) return;

    char path_only[160];
    size_t n = strcspn(path_and_query, "?");
    if (n >= sizeof(path_only)) n = sizeof(path_only) - 1;
    memcpy(path_only, path_and_query, n);
    path_only[n] = '\0';

    time_t now = time(NULL);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));

    fprintf(g_jf_log, "[%s] %-4s %-48s %-4s %6.0fms %6ld bytes\n",
            ts, method ? method : "GET", path_only,
            ok ? "ok" : "FAIL", elapsed_ms, bytes);
    fflush(g_jf_log);   /* request rate is low; a crash right after must not lose this line */
}

/* First CA bundle that actually exists on disk, or NULL. Stock MiSTer curl
 * has no working default CA path — verifying an HTTPS server fails with
 * "unable to get local issuer certificate" even though a perfectly good
 * bundle ships in the image, so a verified request has to point --cacert at
 * one explicitly. (The updater in update.c needs this too and keeps its own
 * copy; kept separate so each translation unit stays self-contained.) */
static const char *jf_ca_bundle(void)
{
    static const char *const cands[] = {
        "/etc/ssl/certs/cacert.pem",
        "/etc/ssl/cert.pem",
        "/usr/lib/python3.9/site-packages/certifi/cacert.pem",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && st.st_size > 0) return cands[i];
    }
    return NULL;
}

/* Appends the TLS-verification flags to a curl argv being built, returning
 * the new element count. INSECURE_TLS in the config => "-k" (skip
 * verification, for a self-signed home server); otherwise verify, pointing
 * --cacert at a real bundle so verification can actually succeed on this
 * platform. Plain http:// requests ignore all of this. */
static int jf_add_tls_args(const JfConfig *cfg, const char **argv, int n)
{
    if (cfg->insecure_tls) { argv[n++] = "-k"; return n; }
    const char *ca = jf_ca_bundle();
    if (ca) { argv[n++] = "--cacert"; argv[n++] = ca; }
    return n;
}

static char *jf_request_alloc(const JfConfig *cfg, const char *method,
                               const char *path_and_query, const char *body_file,
                               int timeout_secs)
{
    char auth[320];
    jf_auth_header(cfg, auth, sizeof(auth));

    char timeout[16];
    snprintf(timeout, sizeof(timeout), "%d", timeout_secs);

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", cfg->server, path_and_query);

    char body_arg[128];
    if (body_file) snprintf(body_arg, sizeof(body_arg), "@%s", body_file);

    /* Built as a plain argv. Each element reaches curl exactly as written,
     * with no quoting, escaping or word splitting anywhere in between. */
    const char *argv[24];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    n = jf_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time";
    argv[n++] = timeout;
    if (method) { argv[n++] = "-X"; argv[n++] = method; }
    argv[n++] = "-H";
    argv[n++] = auth;
    if (body_file) {
        argv[n++] = "-H";
        argv[n++] = "Content-Type: application/json";
        argv[n++] = "--data-binary";
        argv[n++] = body_arg;
    }
    argv[n++] = url;
    argv[n]   = NULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    char *result = jf_curl_run((char *const *)argv, 1, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    jf_log_request(method, path_and_query, ok && result != NULL, elapsed_ms,
                    result ? (long)strlen(result) : 0);

    return result;
}

/* Fetch + parse in one step. Returns 1 on success, with *r owning both the
 * text and the tree; 0 if the request failed or the response wasn't valid
 * JSON (in which case *r is already cleaned up). */
static int jf_fetch(const JfConfig *cfg, const char *path_and_query, JfResponse *r)
{
    memset(r, 0, sizeof(*r));
    r->text = jf_request_alloc(cfg, NULL, path_and_query, NULL, 8);
    if (!r->text) return 0;
    if (!json_parse(&r->doc, r->text)) { jf_response_free(r); return 0; }
    return 1;
}

/* Fire-and-forget POST with a JSON body (Sessions/Playing family). Body is
 * written to a temp file to avoid quoting issues in the curl command line. */
static void jf_post_json(const JfConfig *cfg, const char *path, const char *json_body)
{
    /* mkstemp, not a getpid()-based name: the pid is identical across
     * threads (so two concurrent POSTs would clash) and predictable (so a
     * pre-planted symlink at the path would be followed by this root
     * process). mkstemp creates the file itself with O_EXCL and a random
     * suffix, closing both. */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/misterfin_post_XXXXXX");
    int fd = mkstemp(tmp_path);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return; }
    /* A short write here means a truncated/invalid body, so don't send it —
     * the earlier version could POST a partial JSON payload silently. */
    int ok = (fputs(json_body, f) >= 0);
    if (fclose(f) != 0) ok = 0;

    /* Reuses the same no-shell path as every other request — see
     * jf_curl_run. The response is discarded, but the arguments still carry
     * the server-issued token. */
    if (ok) free(jf_request_alloc(cfg, "POST", path, tmp_path, 5));
    unlink(tmp_path);
}

/* ── config ───────────────────────────────────────────────────────────────── */

int jf_config_load(JfConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->profile_width   = JF_PROFILE_DEFAULT_W;
    cfg->profile_height  = JF_PROFILE_DEFAULT_H;
    cfg->profile_bitrate = JF_PROFILE_DEFAULT_RATE;

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

        /* An optional transcode profile, "WxH" or "WxH@BITRATE" (e.g.
         * "640x480@12000000"). Matched by shape rather than by position for
         * the same reason as PAL/NTSC below — and the shape is unambiguous:
         * a server URL contains "://", and no API key or username is two
         * integers joined by an 'x'. sscanf's %n confirms the whole line was
         * consumed, so "480x270junk" is not silently accepted. */
        {
            int w = 0, h = 0, rate = 0, used = 0;
            if ((sscanf(line, "%dx%d@%d%n", &w, &h, &rate, &used) == 3 && !line[used]) ||
                (sscanf(line, "%dx%d%n", &w, &h, &used) == 2 && !line[used])) {
                if (w >= JF_PROFILE_MIN_W && w <= JF_PROFILE_MAX_W &&
                    h >= JF_PROFILE_MIN_H && h <= JF_PROFILE_MAX_H) {
                    cfg->profile_width  = w;
                    cfg->profile_height = h;
                    if (rate >= JF_PROFILE_MIN_RATE && rate <= JF_PROFILE_MAX_RATE)
                        cfg->profile_bitrate = rate;
                }
                /* Consumed either way — a malformed profile line must not
                 * fall through and be read as an API key. */
                continue;
            }
        }

        /* PAL/NTSC is recognised wherever it appears rather than strictly as
         * the 4th line. The file is positional and blank lines are skipped,
         * so with Quick Connect making the API key and username optional
         * there was no way to write "no key, but NTSC" — the mode would slide
         * up into the api_key slot and be read as a credential. Matching it
         * by value instead keeps every combination expressible, and costs
         * nothing: no server URL, key or username is ever the literal string
         * "PAL" or "NTSC". */
        if (!strcasecmp(line, "PAL") || !strcasecmp(line, "NTSC")) {
            strncpy(cfg->tv_mode, line, sizeof(cfg->tv_mode) - 1);
            continue;
        }

        /* Same "recognised wherever it appears" treatment as PAL/NTSC above,
         * and for the same reason — no server URL, key, username or TV mode
         * is ever literally the string "DEBUGLOG". See jf_log_init. */
        if (!strcasecmp(line, "DEBUGLOG")) {
            cfg->debug_log = 1;
            continue;
        }

        /* Same treatment again — a real config value is never this literal. */
        if (!strcasecmp(line, "INSECURE_TLS")) {
            cfg->insecure_tls = 1;
            continue;
        }

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

    /* An API key in the config file is used as-is; without one the caller
     * falls back to a saved token, then to Quick Connect. */
    strncpy(cfg->token, cfg->api_key, sizeof(cfg->token) - 1);

    /* Only the server URL is required. api_key/username missing used to be a
     * hard failure, which forced every user through the admin dashboard to
     * mint a key — Quick Connect exists precisely so they don't have to. */
    return cfg->server[0] != '\0';
}

int jf_has_credential(const JfConfig *cfg)
{
    return cfg && cfg->token[0] != '\0';
}

/* Rejects control characters (and DEL) anywhere in a credential. Empty is
 * fine — callers check that separately where it matters. */
static int jf_credential_is_sane(const char *s)
{
    if (!s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return 0;
    return 1;
}

/* ── device identity ──────────────────────────────────────────────────────── */

static const char *jf_device_paths[] = {
    "/media/fat/misterfin/device.conf",
    "./device.conf",
    NULL
};

/* A random RFC-4122-shaped v4 GUID, which is what every other Jellyfin client
 * reports. Seeded from /dev/urandom; rand() is the fallback for the
 * essentially-impossible case of that being unavailable, and collisions there
 * would only mean two installs sharing a device entry, not anything unsafe. */
static void jf_generate_device_id(char *out, int outlen)
{
    unsigned char b[16];
    int got = 0;

    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) {
        got = (fread(b, 1, sizeof(b), ur) == sizeof(b));
        fclose(ur);
    }
    if (!got) {
        srand((unsigned)(time(NULL) ^ (getpid() << 16)));
        for (size_t i = 0; i < sizeof(b); i++) b[i] = (unsigned char)(rand() & 0xFF);
    }

    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 1 */

    snprintf(out, outlen,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void jf_device_id_init(JfConfig *cfg)
{
    FILE *f = NULL;
    for (int i = 0; jf_device_paths[i] && !f; i++) f = fopen(jf_device_paths[i], "r");

    if (f) {
        char line[80] = {0};
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            /* Sanitised on the way in: it's read from disk, and it ends up
             * inside a single-quoted shell argument. Generated ids only ever
             * contain hex and dashes, so this is a no-op for them and only
             * bites a hand-corrupted file. */
            jf_sanitize_id(line, cfg->device_id, sizeof(cfg->device_id));
        }
        fclose(f);
        if (cfg->device_id[0]) return;
    }

    jf_generate_device_id(cfg->device_id, sizeof(cfg->device_id));

    /* Prefer the real install location; fall back to the working directory
     * when it isn't there, same as the token file. */
    const char *path = jf_device_paths[1];
    FILE *probe = fopen("/media/fat/misterfin/jellyfin.conf", "r");
    if (probe) { fclose(probe); path = jf_device_paths[0]; }

    FILE *w = fopen(path, "w");
    if (w) { fprintf(w, "%s\n", cfg->device_id); fclose(w); }
    /* A write failure is survivable — the id just won't persist, so the
     * server sees a new device next launch. Not worth failing startup over. */
}

/* ── saved token ──────────────────────────────────────────────────────────── */

/* Written next to jellyfin.conf, with the desktop fallback the config loader
 * uses so off-hardware runs don't try to write to /media/fat. Two lines:
 * access token, then user id — the id is stored rather than re-resolved
 * because /Users needs elevated access that a plain user token doesn't have. */
static const char *jf_token_paths[] = {
    "/media/fat/misterfin/token.conf",
    "./token.conf",
    NULL
};

static const char *jf_token_writable_path(void)
{
    /* Prefer the real install location, but fall back to the working
     * directory when it isn't there (desktop runs). */
    FILE *probe = fopen("/media/fat/misterfin/jellyfin.conf", "r");
    if (probe) { fclose(probe); return jf_token_paths[0]; }
    return jf_token_paths[1];
}

int jf_token_save(const JfConfig *cfg)
{
    if (!cfg->token[0]) return 0;
    const char *path = jf_token_writable_path();
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%s\n%s\n%s\n", cfg->token, cfg->user_id, cfg->username);
    fclose(f);
    return 1;
}

int jf_token_load(JfConfig *cfg)
{
    FILE *f = NULL;
    for (int i = 0; jf_token_paths[i] && !f; i++) f = fopen(jf_token_paths[i], "r");
    if (!f) return 0;

    char token[192] = {0}, user_id[JF_ID_LEN] = {0}, username[64] = {0};
    int ok = 0;
    if (fgets(token, sizeof(token), f)) {
        token[strcspn(token, "\r\n")] = '\0';
        if (fgets(user_id, sizeof(user_id), f)) {
            user_id[strcspn(user_id, "\r\n")] = '\0';
            if (fgets(username, sizeof(username), f))
                username[strcspn(username, "\r\n")] = '\0';
            ok = token[0] && user_id[0];
        }
    }
    fclose(f);
    if (!ok) return 0;

    strncpy(cfg->token,   token,   sizeof(cfg->token) - 1);
    strncpy(cfg->user_id, user_id, sizeof(cfg->user_id) - 1);
    if (username[0]) strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    return 1;
}

void jf_token_clear(void)
{
    for (int i = 0; jf_token_paths[i]; i++) unlink(jf_token_paths[i]);
}

/* ── Quick Connect ────────────────────────────────────────────────────────── */

/* POST + parse, for the two Quick Connect calls that return a body.
 * json_body may be NULL (Initiate takes no body at all). */
static int jf_post_fetch(const JfConfig *cfg, const char *path,
                          const char *json_body, JfResponse *r)
{
    memset(r, 0, sizeof(*r));

    char tmp_path[64] = "";
    if (json_body) {
        /* mkstemp for the same reason as jf_post_json — unique per call,
         * not a followable predictable path. */
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/misterfin_qc_XXXXXX");
        int fd = mkstemp(tmp_path);
        if (fd < 0) return 0;
        FILE *f = fdopen(fd, "w");
        if (!f) { close(fd); unlink(tmp_path); return 0; }
        int ok = (fputs(json_body, f) >= 0);
        if (fclose(f) != 0) ok = 0;
        if (!ok) { unlink(tmp_path); return 0; }
    }

    r->text = jf_request_alloc(cfg, "POST", path, json_body ? tmp_path : NULL, 10);
    if (json_body) unlink(tmp_path);

    if (!r->text) return 0;
    if (!json_parse(&r->doc, r->text)) { jf_response_free(r); return 0; }
    return 1;
}

int jf_quick_connect_enabled(const JfConfig *cfg)
{
    /* Returns a bare `true`/`false` rather than an object — still valid JSON,
     * so the parser handles it and the root node is the boolean itself. */
    JfResponse r;
    if (!jf_fetch(cfg, "/QuickConnect/Enabled", &r)) return -1;
    int enabled = json_as_bool(json_root(&r.doc), 0);
    jf_response_free(&r);
    return enabled ? 1 : 0;
}

int jf_quick_connect_initiate(const JfConfig *cfg, JfQuickConnect *qc)
{
    memset(qc, 0, sizeof(*qc));

    JfResponse r;
    if (!jf_post_fetch(cfg, "/QuickConnect/Initiate", NULL, &r)) return 0;

    json_copy_str(&r.doc, NULL, "Secret", qc->secret, sizeof(qc->secret));
    json_copy_str(&r.doc, NULL, "Code",   qc->code,   sizeof(qc->code));
    jf_response_free(&r);

    return qc->secret[0] && qc->code[0];
}

int jf_quick_connect_poll(const JfConfig *cfg, const JfQuickConnect *qc)
{
    /* The secret is server-generated and goes into a URL, so it gets the same
     * allowlist treatment as any other id from the server. */
    char safe_secret[128];
    jf_sanitize_id(qc->secret, safe_secret, sizeof(safe_secret));

    char path[192];
    snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", safe_secret);

    JfResponse r;
    /* A failed request here is ambiguous — 404 means the server has forgotten
     * this request (expired or denied), but so does a dropped connection.
     * Reported as -1 either way; the caller retries a couple of times before
     * giving up, so a transient blip doesn't abandon a live request. */
    if (!jf_fetch(cfg, path, &r)) return -1;

    int authed = json_as_bool(json_find(&r.doc, NULL, "Authenticated"), 0);
    jf_response_free(&r);
    return authed ? 1 : 0;
}

int jf_quick_connect_authenticate(JfConfig *cfg, const JfQuickConnect *qc)
{
    char body[192];
    snprintf(body, sizeof(body), "{\"Secret\":\"%s\"}", qc->secret);

    JfResponse r;
    if (!jf_post_fetch(cfg, "/Users/AuthenticateWithQuickConnect", body, &r)) return 0;

    char token[sizeof(cfg->token)] = {0};
    char user_id[JF_ID_LEN] = {0};
    char username[64] = {0};
    json_copy_str(&r.doc, NULL, "AccessToken",  token,    sizeof(token));
    json_copy_str(&r.doc, NULL, "User.Id",      user_id,  sizeof(user_id));
    json_copy_str(&r.doc, NULL, "User.Name",    username, sizeof(username));
    jf_response_free(&r);

    if (!token[0] || !user_id[0]) return 0;

    /* A credential is opaque to us, so there's nothing to validate about its
     * content — except that it must not contain control characters. token.conf
     * is line-oriented, and a newline inside the token (which the server can
     * send as
, and which this client now decodes faithfully) would
     * write a file that reloads as a different, truncated credential: a
     * permanent self-inflicted lockout. Rejecting outright beats storing
     * something that silently means something else. */
    if (!jf_credential_is_sane(token) || !jf_credential_is_sane(user_id) ||
        !jf_credential_is_sane(username))
        return 0;

    strncpy(cfg->token,   token,   sizeof(cfg->token) - 1);
    strncpy(cfg->user_id, user_id, sizeof(cfg->user_id) - 1);
    /* Whoever approved the request IS the user — no /Users lookup needed, and
     * no need for the username in the config file to match (or be there). */
    if (username[0]) strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    return 1;
}

int jf_credential_works(const JfConfig *cfg)
{
    if (!jf_has_credential(cfg) || !cfg->user_id[0]) return 0;

    /* UserViews is the smallest authenticated, user-scoped call available —
     * it fails for a revoked token but doesn't need elevated access the way
     * /Users does. */
    char path[256];
    snprintf(path, sizeof(path), "/UserViews?userId=%s", cfg->user_id);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return 0;
    int ok = json_find(&r.doc, NULL, "Items") != NULL;
    jf_response_free(&r);
    return ok;
}

int jf_resolve_user_id(JfConfig *cfg)
{
    JfResponse r;
    if (!jf_fetch(cfg, "/Users", &r)) return -1;

    /* /Users returns a bare array, so the root node is what to iterate. */
    int result = 0;
    JSON_FOREACH(&r.doc, json_root(&r.doc), user) {
        const char *name = json_as_str(json_find(&r.doc, user, "Name"), "");
        if (strcasecmp(name, cfg->username) != 0) continue;
        json_copy_str(&r.doc, user, "Id", cfg->user_id, sizeof(cfg->user_id));
        result = cfg->user_id[0] != '\0';
        break;
    }

    jf_response_free(&r);
    return result;
}

/* ── browsing ─────────────────────────────────────────────────────────────── */

int jf_list_views(const JfConfig *cfg, JfItem *out, int max)
{
    char path[256];
    snprintf(path, sizeof(path), "/UserViews?userId=%s", cfg->user_id);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_FOLDER;
    jf_response_free(&r);
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

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;
    int64_t result = json_as_i64(json_find(&r.doc, NULL, "TotalRecordCount"), -1);
    jf_response_free(&r);
    return result;
}

/* Reads TotalRecordCount into *total_out (may be NULL), leaving it untouched
 * if the server didn't send one — the caller then falls back to however many
 * items actually arrived. */
static void parse_total_count(const JsonDoc *doc, int64_t *total_out)
{
    if (!total_out) return;
    const JsonNode *n = json_find(doc, NULL, "TotalRecordCount");
    if (n && n->type == JSON_NUMBER) *total_out = n->i64;
}

int jf_list_items(const JfConfig *cfg, const char *parent_id, int start_index,
                   JfItem *out, int max, int64_t *total_out)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));
    if (start_index < 0) start_index = 0;

    /* ChildCount here (unlike on a top-level library view — see
     * jf_count_items's comment) is exactly what it sounds like for a
     * MusicAlbum: its own track count, confirmed against a real server. */
    /* Overview is deliberately NOT requested here even though JfItem has a
     * field for it: no browse-list row ever displays it (the info screen
     * re-fetches via jf_get_item_details, which is where overview actually
     * comes from), so it would be several times the JSON per row for nothing.
     * Response size no longer risks losing data the way it once did — buffers
     * grow to fit and a short read is now a hard parse failure — but there's
     * still no reason to transfer and parse a paragraph per row that nothing
     * reads. */
    char path[512];
    snprintf(path, sizeof(path),
        "/Items?userId=%s&ParentId=%s&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount,RecursiveItemCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary&StartIndex=%d&Limit=%d",
        cfg->user_id, safe_parent, start_index, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;   /* transport/parse failure, not an empty page */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    jf_response_free(&r);
    return n;
}

int jf_list_items_recursive(const JfConfig *cfg, const char *parent_id,
                             const char *item_type, JfItem *out, int max)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    /* No Overview here either — see jf_list_items. This one only ever feeds
     * the carousel's background cover grid, which needs nothing but ids and
     * image tags. */
    char path[560];
    if (item_type && item_type[0])
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true&IncludeItemTypes=%s"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, item_type, max);
    else
        snprintf(path, sizeof(path),
            "/Items?userId=%s&ParentId=%s&Recursive=true"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    jf_response_free(&r);
    return n;
}

int jf_list_random_tracks(const JfConfig *cfg, const char *parent_id, JfItem *out, int max)
{
    char safe_parent[JF_ID_LEN];
    jf_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    char path[512];
    snprintf(path, sizeof(path),
        "/Items?userId=%s&ParentId=%s&Recursive=true&IncludeItemTypes=Audio&SortBy=Random"
        "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
        cfg->user_id, safe_parent, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    jf_response_free(&r);
    return n;
}

/* Shared field list for the two home rows. Both want enough to draw a browse
 * row (year, runtime, watched/resume state, a cover) and nothing more. */
#define JF_HOME_ROW_FIELDS \
    "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true" \
    "&ImageTypeLimit=1&EnableImageTypes=Primary&EnableTotalRecordCount=true"

int jf_list_resume(const JfConfig *cfg, JfItem *out, int max, int64_t *total_out)
{
    /* MediaTypes=Video keeps half-listened music out of a row the user reads
     * as "films and episodes I'm partway through". Jellyfin orders these by
     * DatePlayed descending server-side, which is the order to preserve. */
    char path[384];
    snprintf(path, sizeof(path),
        "/UserItems/Resume?userId=%s&MediaTypes=Video&Limit=%d" JF_HOME_ROW_FIELDS,
        cfg->user_id, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;   /* transport/parse failure, not an empty page */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    jf_response_free(&r);
    return n;
}

int jf_list_nextup(const JfConfig *cfg, JfItem *out, int max, int64_t *total_out)
{
    /* enableResumable=false: a part-watched episode already appears under
     * Continue Watching, and having the same episode in both rows makes the
     * two look broken rather than complementary. Next Up is then strictly
     * "what to start next". */
    char path[384];
    snprintf(path, sizeof(path),
        "/Shows/NextUp?userId=%s&Limit=%d&enableResumable=false" JF_HOME_ROW_FIELDS,
        cfg->user_id, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;   /* see jf_list_items */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    /* NextUp returns episodes by definition, but says so only via each item's
     * own Type — set it explicitly for the same reason jf_list_episodes does. */
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_EPISODE;
    jf_response_free(&r);
    return n;
}

int jf_get_item_details(const JfConfig *cfg, const char *item_id, JfItem *out)
{
    char safe_id[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));

    char path[384];
    snprintf(path, sizeof(path),
        "/Items/%s?userId=%s&Fields=Overview,ProductionYear,RunTimeTicks,People,MediaStreams,CommunityRating"
        "&EnableUserData=true&EnableImageTypes=Primary,Logo,Backdrop",
        safe_id, cfg->user_id);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return 0;

    /* A single-item fetch returns the item object itself as the root, so all
     * three parsers are scoped to it rather than to a list element. */
    const JsonNode *item = json_root(&r.doc);
    parse_item_fields(&r.doc, item, out);
    parse_cast(&r.doc, item, out);
    parse_media_streams(&r.doc, item, out);
    jf_response_free(&r);
    return 1;
}

int jf_list_seasons(const JfConfig *cfg, const char *series_id, JfItem *out, int max)
{
    char safe_series[JF_ID_LEN];
    jf_sanitize_id(series_id, safe_series, sizeof(safe_series));

    char path[256];
    snprintf(path, sizeof(path), "/Shows/%s/Seasons?userId=%s", safe_series, cfg->user_id);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;   /* see jf_list_items */
    int n = parse_item_list(&r.doc, out, max);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_SEASON;
    jf_response_free(&r);
    return n;
}

int jf_list_episodes(const JfConfig *cfg, const char *series_id, const char *season_id,
                      int start_index, JfItem *out, int max, int64_t *total_out)
{
    char safe_series[JF_ID_LEN], safe_season[JF_ID_LEN];
    jf_sanitize_id(series_id, safe_series, sizeof(safe_series));
    jf_sanitize_id(season_id, safe_season, sizeof(safe_season));
    if (start_index < 0) start_index = 0;

    char path[416];
    snprintf(path, sizeof(path),
        "/Shows/%s/Episodes?seasonId=%s&userId=%s"
        "&Fields=RunTimeTicks&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary&StartIndex=%d&Limit=%d",
        safe_series, safe_season, cfg->user_id, start_index, max);

    JfResponse r;
    if (!jf_fetch(cfg, path, &r)) return -1;   /* see jf_list_items */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    for (int i = 0; i < n; i++) out[i].type = JF_TYPE_EPISODE;
    jf_response_free(&r);
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

    /* No shell — see jf_curl_run. The URL embeds an image tag that came
     * from server JSON. -L follows redirects; TLS verification per config
     * (see jf_add_tls_args). */
    const char *argv[12];
    int n = 0;
    argv[n++] = "curl"; argv[n++] = "-sfL";
    n = jf_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time"; argv[n++] = "15";
    argv[n++] = url; argv[n++] = "-o"; argv[n++] = dest_path;
    argv[n] = NULL;
    int ok = 0;
    jf_curl_run((char *const *)argv, 0, &ok);
    return ok;
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
                   int audio_stream_index,
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

    /* Omitted entirely rather than guessed when no explicit track is wanted,
     * so the server applies the source's own default selection. */
    char audio_params[32] = "";
    if (audio_stream_index >= 0)
        snprintf(audio_params, sizeof(audio_params),
                 "&audioStreamIndex=%d", audio_stream_index);

    /* allowVideoStreamCopy=false: without it, a source that already matches
     * the request (an MPEG-2 DVD rip whose 720x576 fits inside the profile
     * box) gets stream-COPIED instead of transcoded — confirmed on hardware
     * with two PAL DVD rips at a 720x576 profile: severe slowdown plus
     * codec-looking glitches without this parameter, clean playback with it,
     * same titles/profile/session (note /Sessions' TranscodingInfo can't
     * tell the two apart — it comes back empty for ApiKey progressive
     * streams either way). The whole playback pipeline here is built on the
     * server delivering a clean, progressive, freshly-encoded stream (the
     * mplayer chain has no deinterlacer, and raw DVD MPEG-2 is interlaced
     * VBR with DVD-style timestamps) — so forbid the copy shortcut
     * outright; a genuine re-encode of an already-small source is cheap for
     * the server anyway. Audio needs no equivalent: audioCodec=mp3 already
     * forces a real audio transcode, see the comment above. */
    snprintf(out, outlen,
        "%s/Videos/%s/stream?static=false&videoCodec=mpeg2video&container=ts"
        "&audioCodec=mp3&audioChannels=2&allowVideoStreamCopy=false"
        "&maxWidth=%d&maxHeight=%d&videoBitRate=%d"
        "&startTimeTicks=%lld&playSessionId=%s%s%s&ApiKey=%s",
        cfg->server, safe_id,
        profile->max_width, profile->max_height, profile->video_bitrate,
        (long long)(start_ticks > 0 ? start_ticks : 0),
        safe_session, sub_params, audio_params, cfg->token);
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
        cfg->server, safe_id, safe_session, cfg->token);
    return 1;
}

int jf_download_subtitle(const JfConfig *cfg, const char *item_id,
                          const char *media_source_id, int sub_index,
                          const char *dest_path)
{
    char safe_id[JF_ID_LEN], safe_msid[JF_ID_LEN];
    jf_sanitize_id(item_id, safe_id, sizeof(safe_id));
    jf_sanitize_id(media_source_id, safe_msid, sizeof(safe_msid));

    /* Sized for a full server URL plus a 191-byte token: 384 bytes could
     * truncate the ApiKey, which showed up as subtitles silently failing to
     * load rather than as any visible error. */
    char url[768];
    snprintf(url, sizeof(url), "%s/Videos/%s/%s/Subtitles/%d/Stream.srt?ApiKey=%s",
              cfg->server, safe_id, safe_msid, sub_index, cfg->token);

    const char *argv[12];
    int n = 0;
    argv[n++] = "curl"; argv[n++] = "-sfL";
    n = jf_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time"; argv[n++] = "15";
    argv[n++] = url; argv[n++] = "-o"; argv[n++] = dest_path;
    argv[n] = NULL;
    int ok = 0;
    jf_curl_run((char *const *)argv, 0, &ok);
    return ok;
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
                        const char *play_session_id, int64_t position_ticks,
                        int played)
{
    (void)play_session_id;
    /* When the title was watched to the end, Jellyfin's convention is
     * Played=true with the resume position cleared to 0. Reporting the
     * near-end position with Played=false unconditionally (what this used to
     * do) left every finished title still offering a resume that jumped
     * straight to the last few seconds — and kept it sitting in the Continue
     * Watching row instead of moving the series on to Next Up. */
    report_user_data(cfg, item_id, played ? 0 : position_ticks, played);
}
