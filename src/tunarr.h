#pragma once
#include <stdint.h>
#include <time.h>

/* Tunarr REST client — same shape as jellyfin.c/h (curl shelled out via
 * execv, no libcurl dependency, hand-rolled JSON parsing via json.c) but
 * deliberately independent of it: this file must not #include "jellyfin.h"
 * or call any jf_* function, so a Tunarr-only feature never drags Jellyfin
 * concepts (auth, resume state, subtitle tracks, ...) along with it.
 *
 * Confirmed against Tunarr's own OpenAPI spec (docs/generated/
 * tunarr-latest-openapi.json in the tunarr repo): no endpoint used here
 * requires authentication, so there is no credential/token handling at all
 * — a real simplification compared to jellyfin.h, not an oversight. */

#define TUNARR_ID_LEN     64
#define TUNARR_NAME_LEN   128
#define TUNARR_TITLE_LEN  128
#define TUNARR_MAX_CHANNELS 128

/* How many upcoming programs (starting with whatever's airing "now") each
 * channel's schedule[] carries — a short forward-only chain, not the whole
 * multi-day xmltv.xml lineup: enough for L/R schedule-browsing in the guide
 * screen to page a handful of programs ahead, without every one of up to
 * 128 channels' full arrays becoming a real memory line item. */
#define TUNARR_SCHEDULE_MAX 6

typedef struct {
    char server[256];   /* e.g. http://192.168.2.20:8000 (no trailing slash) */
} TunarrConfig;

typedef struct {
    char    title[TUNARR_TITLE_LEN];
    time_t  start;
    time_t  stop;
    /* Tunarr-proxied artwork URL for THIS specific program (e.g.
     * ".../api/programs/<id>/artwork/poster") — captured per-entry straight
     * from xmltv.xml's <programme><icon src=.../>, so browsing the schedule
     * with L/R can swap the guide screen's backdrop to match whichever
     * program is currently shown, at no extra network cost. Empty if that
     * listing had none. */
    char    poster_url[256];
} TunarrScheduleItem;

typedef struct {
    char    id[TUNARR_ID_LEN];
    int     number;
    char    name[TUNARR_NAME_LEN];
    char    icon_path[256];    /* Channel.icon.path — captured, unused until
                                 * icons are added (deferred past v1) */
    char    group_title[64];   /* Channel.groupTitle — captured, unused until
                                 * grouping/filtering is added (deferred past v1) */
    /* schedule[0] is whatever's airing "now" (or, rarely, the next thing
     * airing if there's a gap in this channel's lineup right now — nothing
     * here assumes schedule[0].start <= now), schedule[1..] are what's
     * coming up after it, in order. Filled by tunarr_get_guide(); both this
     * array and schedule_count are 0 until that's called —
     * tunarr_list_channels() alone does not populate these. */
    TunarrScheduleItem schedule[TUNARR_SCHEDULE_MAX];
    int                schedule_count;
} TunarrChannel;

/* Loads /media/fat/misterfin/tunarr.conf (falls back to ./tunarr.conf for
 * desktop testing, same two-path pattern as jf_config_load). One line: the
 * server URL — no auth line, since no Tunarr endpoint this client uses
 * requires a credential. Returns 1 if a non-empty server URL was found, 0
 * otherwise (missing file or empty first line) — 0 means the caller should
 * treat Tunarr as unavailable and leave the feature out of the root menu. */
int tunarr_config_load(TunarrConfig *cfg);

/* GET /api/channels — the channel list, unordered by the server (sorted by
 * number here so channel-up/down moves in a sane direction). Does NOT fill
 * schedule[]/schedule_count — call tunarr_get_guide() afterward for those.
 * Returns the count written to out (may be 0), or -1 if the request itself
 * failed. */
int tunarr_list_channels(const TunarrConfig *cfg, TunarrChannel *out, int max);

/* GET /api/xmltv.xml ONCE for every channel in `chans`, instead of a
 * per-channel REST round trip — Tunarr's XMLTV export (the same feed a Kodi
 * IPTV Simple Client EPG import would use) already carries the full
 * multi-day schedule for every channel in one document, so one fetch fills
 * in schedule[]/schedule_count (up to TUNARR_SCHEDULE_MAX programs, starting
 * with whatever's airing now) for every channel in `chans`, matched by
 * TunarrChannel.number against xmltv's `channel="C<number>.<id>.tunarr.com"`
 * attribute (its own internal id after the number is irrelevant here — the
 * channel number is the join key back to what tunarr_list_channels()
 * already gave us). Hand-rolled XMLTV scanning (find <programme> tags + a
 * handful of attributes/entities), not a general XML parser — mirrors
 * json.c's own "small, complete enough for this one shape" philosophy.
 *
 * Clears every channel's schedule[]/schedule_count in `chans` before
 * scanning (unlike a per-channel call, a transient failure here means the
 * WHOLE guide is stale, not just one channel, so there's no meaningful
 * partial state worth preserving) and returns 1 on a successful
 * fetch+parse, 0 if the request itself failed (chans left cleared either
 * way). */
int tunarr_get_guide(const TunarrConfig *cfg, TunarrChannel *chans, int n);

/* Downloads an artwork URL (TunarrScheduleItem.poster_url above — Tunarr's
 * own artwork-proxy endpoint, e.g. ".../api/programs/<id>/artwork/poster")
 * to dest_path for use as a decorative backdrop. That endpoint has no size
 * parameter of its own — it 302s straight to the full-resolution original (Jellyfin/Plex/
 * Emby, whichever the source is). This resolves that redirect itself and,
 * when it lands on a Jellyfin URL (recognizable by its own maxWidth-capable
 * query string), requests a copy resized to max_width pixels wide instead
 * of the original — matching jf_download_item_image()'s own convention of
 * never decoding a source image at full resolution (a MiSTer's HPS memory
 * isn't budgeted for that). Any other source, or a response that isn't a
 * redirect at all, falls back to a plain fetch of `url` as given — heavier
 * on memory, but still correct for a decorative background. Returns 1 on
 * success. Decode the result with load_image_tmp() (util.h), same as every
 * other downloaded image in this codebase. */
int tunarr_download_image(const char *url, int max_width, const char *dest_path);

/* Builds http://<server>/stream/channels/<id>.ts — a continuous direct
 * MPEG-TS feed (confirmed: no auth, no required query params). This is the
 * URL handed to mplayer, the same way jf_stream_url()'s result is. */
void tunarr_stream_url(const TunarrConfig *cfg, const char *channel_id,
                        char *out, int outlen);
