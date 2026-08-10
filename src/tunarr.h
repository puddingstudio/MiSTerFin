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

typedef struct {
    char server[256];   /* e.g. http://192.168.2.20:8000 (no trailing slash) */
} TunarrConfig;

typedef struct {
    char    id[TUNARR_ID_LEN];
    int     number;
    char    name[TUNARR_NAME_LEN];
    char    icon_path[256];    /* Channel.icon.path — captured, unused until
                                 * icons are added (deferred past v1) */
    char    group_title[64];   /* Channel.groupTitle — captured, unused until
                                 * grouping/filtering is added (deferred past v1) */
    /* Current program, filled by tunarr_get_now_playing(). Empty/0 until
     * that's called — tunarr_list_channels() alone does not populate these. */
    char    program_title[TUNARR_TITLE_LEN];
    time_t  program_start;   /* 0 if not fetched */
    time_t  program_stop;    /* 0 if not fetched */
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
 * program_title/program_start/program_stop — call tunarr_get_now_playing()
 * per channel afterward for that. Returns the count written to out (may be
 * 0), or -1 if the request itself failed. */
int tunarr_list_channels(const TunarrConfig *cfg, TunarrChannel *out, int max);

/* GET /api/channels/:id/now_playing — fills in ch->program_title/start/stop
 * in place. Handles all 4 response shapes Tunarr can return (confirmed
 * against its OpenAPI spec): "content" (title at program.title), "custom"
 * (title at program.program.title — a custom show wraps another content
 * program), "flex" (filler slate, title at the response's own top level),
 * and "redirect" (this channel is currently just relaying another one —
 * program_title becomes "-> <redirected channel's name>"). Returns 1 on
 * success, 0 on failure (network or parse) — ch's program_* fields are left
 * untouched on failure rather than cleared, so a transient miss doesn't
 * blank out the last-known program title on screen. */
int tunarr_get_now_playing(const TunarrConfig *cfg, TunarrChannel *ch);

/* Builds http://<server>/stream/channels/<id>.ts — a continuous direct
 * MPEG-TS feed (confirmed: no auth, no required query params). This is the
 * URL handed to mplayer, the same way jf_stream_url()'s result is. */
void tunarr_stream_url(const TunarrConfig *cfg, const char *channel_id,
                        char *out, int outlen);
