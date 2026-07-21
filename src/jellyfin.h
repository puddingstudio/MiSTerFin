#pragma once
#include <stdint.h>

/* Jellyfin REST client — curl shelled out via popen(), no libcurl dependency
 * (matches MiSTerDVD's approach). All endpoints/params below were verified
 * against the official Jellyfin OpenAPI spec (see plan doc); a few optional
 * fields (MediaSourceId, PlaybackInfo negotiation) are intentionally omitted
 * rather than guessed — see README "Known limitations". */

#define JF_MAX_ITEMS   256
#define JF_ID_LEN      40
#define JF_NAME_LEN    256
#define JF_OVERVIEW_LEN 1024
#define JF_PERSON_NAME_LEN 64
#define JF_MAX_CAST    8
#define JF_MAX_SUBS    8
#define JF_SUB_LABEL_LEN 48

typedef enum {
    JF_TYPE_FOLDER,   /* library view / generic folder */
    JF_TYPE_SERIES,
    JF_TYPE_SEASON,
    JF_TYPE_EPISODE,
    JF_TYPE_MOVIE,
    JF_TYPE_ARTIST,   /* MusicArtist — drills into albums, browsed like JF_TYPE_FOLDER */
    JF_TYPE_ALBUM,    /* MusicAlbum — drills into tracks, browsed like JF_TYPE_FOLDER */
    JF_TYPE_TRACK,    /* Audio — playable leaf, like JF_TYPE_MOVIE but audio-only */
    JF_TYPE_OTHER
} JfItemType;

typedef struct {
    char id[JF_ID_LEN];
    char name[JF_PERSON_NAME_LEN];
    char role[JF_PERSON_NAME_LEN];      /* character/role, empty if n/a */
    char image_tag[JF_ID_LEN];          /* PrimaryImageTag, empty if none */
} JfPerson;

typedef struct {
    int  index;                        /* MediaStreams[].Index — pass as subtitleStreamIndex */
    char label[JF_SUB_LABEL_LEN];       /* Language if set, else DisplayTitle */
    char codec[16];                    /* MediaStreams[].Codec, e.g. "subrip", "ass",
                                         * "pgssub", "dvdsub" — see jf_subtitle_is_text() */
} JfSubtitle;

typedef struct {
    char       id[JF_ID_LEN];
    char       name[JF_NAME_LEN];
    char       overview[JF_OVERVIEW_LEN];
    char       image_tag[JF_ID_LEN];    /* ImageTags.Primary, falling back to
                                          * AlbumPrimaryImageTag for a track that
                                          * doesn't carry its own embedded art (which
                                          * is normal — plenty of files have none, the
                                          * album cover is still the right thing to
                                          * show). Fetch from image_item_id, not id. */
    char       image_item_id[JF_ID_LEN]; /* == id normally; == AlbumId when image_tag
                                           * came from the AlbumPrimaryImageTag fallback */
    char       backdrop_tag[JF_ID_LEN]; /* BackdropImageTags[0], falling back to
                                          * ParentBackdropImageTags[0] for episodes/
                                          * seasons that don't carry their own (which
                                          * is normal — that art is series-level).
                                          * Fetch from backdrop_item_id, not id. */
    char       logo_tag[JF_ID_LEN];     /* ImageTags.Logo, falling back to
                                          * ParentLogoImageTag for the same reason.
                                          * Fetch from logo_item_id, not id. */
    char       backdrop_item_id[JF_ID_LEN]; /* == id normally; == ParentBackdropItemId
                                              * when backdrop_tag came from the fallback */
    char       logo_item_id[JF_ID_LEN];     /* == id normally; == ParentLogoItemId
                                              * when logo_tag came from the fallback */
    char       year[8];
    char       album[JF_NAME_LEN];   /* Album — tracks only, empty otherwise */
    char       artist[JF_NAME_LEN];  /* AlbumArtist — tracks only, empty otherwise */
    char       collection_type[16];  /* CollectionType — "movies"/"tvshows"/"music"/...,
                                       * library views only (jf_list_views), empty otherwise */
    JfItemType type;
    int64_t    runtime_ticks;          /* RunTimeTicks, 0 if unknown */
    int        child_count;            /* ChildCount — track count for a MusicAlbum
                                         * (jf_list_items only requests this field);
                                         * 0/unset for every other item type. */
    int64_t    resume_ticks;           /* UserData.PlaybackPositionTicks */
    int        played;                 /* UserData.Played */
    int        index_number;           /* episode/season number, -1 if n/a */
    JfPerson   cast[JF_MAX_CAST];       /* Actors only, empty unless fetched via jf_get_item_details */
    int        cast_count;
    JfSubtitle subs[JF_MAX_SUBS];       /* empty unless fetched via jf_get_item_details */
    int        sub_count;
    /* Source (original file) video specs — from MediaStreams, empty unless
     * fetched via jf_get_item_details. What's actually streamed to the
     * device is different (see JfStreamProfile) — this is for display only. */
    char       source_video_codec[16];
    int        source_width, source_height;
    int64_t    source_bitrate;          /* bits/sec, 0 if unknown */
} JfItem;

typedef struct {
    char server[256];   /* e.g. http://192.168.2.10:8096 (no trailing slash) */
    char api_key[64];
    char username[64];  /* config line 3 — resolved to user_id via jf_resolve_user_id() */
    char user_id[JF_ID_LEN];   /* empty until resolved; everything else needs this, not username */
    char tv_mode[8];     /* "PAL" (default) or "NTSC" — 4th, optional config line */
} JfConfig;

/* Loads /media/fat/misterfin/jellyfin.conf (falls back to ./jellyfin.conf for
 * desktop testing). 4 lines: server, api_key, username, tv_mode (optional,
 * defaults to PAL). Returns 1 on success, 0 if missing/incomplete.
 * cfg->user_id is NOT populated here — call jf_resolve_user_id() once at
 * startup (it needs a network round-trip). */
int jf_config_load(JfConfig *cfg);

/* Looks up cfg->username in GET /Users and fills in cfg->user_id. The raw
 * Jellyfin user id (a GUID) is not something a user can easily find in the
 * Jellyfin web UI (it only shows up in a dashboard URL) — a plain username
 * is what everyone actually knows, so the client resolves it instead of
 * asking for the id directly. Returns 1 on success (username found), 0
 * otherwise (typo, or user genuinely doesn't exist). */
int jf_resolve_user_id(JfConfig *cfg);

/* Top-level library views (Movies, TV Shows, ...). */
int jf_list_views(const JfConfig *cfg, JfItem *out, int max);

/* Recursive item count under parent_id, filtered to item_type if non-NULL/
 * non-empty (a BaseItemKind string — "Movie", "Series", "MusicAlbum", ...),
 * else an unfiltered recursive count. Limit=0 so the server only computes
 * TotalRecordCount and serializes no items — confirmed against a real
 * server that this is cheap even recursively. Used for the library
 * carousel's "N movies/series/albums" line — the view's own ChildCount
 * field (from jf_list_views) is NOT this: confirmed on a real server it
 * counts something else entirely (e.g. read 6 for a music library whose
 * root folder has exactly 1 direct child). Returns -1 on failure. */
int64_t jf_count_items(const JfConfig *cfg, const char *parent_id, const char *item_type);

/* Direct children of parent_id (a view, a folder, ...). */
int jf_list_items(const JfConfig *cfg, const char *parent_id, JfItem *out, int max);

/* Same shape as jf_list_items but recursive, and filtered to item_type if
 * non-NULL/non-empty (a BaseItemKind string, e.g. "MusicAlbum"). Used by
 * the carousel's background cover grid to reach real leaf-level art (movie/
 * series/album covers) even for libraries organized in intermediate folders
 * — a by-artist music library's direct children are MusicArtist folders,
 * which often carry no cover art of their own; a plain non-recursive
 * listing there found nothing to show (confirmed against a real server). */
int jf_list_items_recursive(const JfConfig *cfg, const char *parent_id,
                             const char *item_type, JfItem *out, int max);

/* TV hierarchy. */
int jf_list_seasons(const JfConfig *cfg, const char *series_id, JfItem *out, int max);
int jf_list_episodes(const JfConfig *cfg, const char *series_id, const char *season_id,
                      JfItem *out, int max);

/* Fetches full details for a single item — Overview, People (cast), and the
 * Backdrop/Logo image tags — that jf_list_items() deliberately omits to keep
 * browse-list fetches cheap (no point pulling 20 actors per movie for every
 * row in a list). Call this once when entering the info screen. */
int jf_get_item_details(const JfConfig *cfg, const char *item_id, JfItem *out);

/* Builds the primary image URL for an item, capped at 200px wide server-side
 * (empty image_tag => returns 0). */
int jf_image_url(const JfConfig *cfg, const JfItem *item, char *out, int outlen);

/* Downloads the primary image to dest_path (curl -o). Returns 1 on success. */
int jf_download_image(const JfConfig *cfg, const JfItem *item, const char *dest_path);

/* General form for any image type ("Primary", "Logo", "Backdrop/0", a cast
 * member's own Primary, ...) at any server-side-resized max_width (<=0 for
 * original size — avoid that; see jf_item_image_url comment in jellyfin.c). */
int jf_item_image_url(const JfConfig *cfg, const char *item_id, const char *image_type,
                       const char *tag, int max_width, char *out, int outlen);
int jf_download_item_image(const JfConfig *cfg, const char *item_id, const char *image_type,
                            const char *tag, int max_width, const char *dest_path);

typedef struct {
    int max_width;
    int max_height;
    int video_bitrate;   /* bits/sec */
} JfStreamProfile;

/* Builds a server-transcoded progressive stream URL for direct HTTP playback
 * (mplayer opens this URL as-is — ApiKey is passed as a query param here
 * since mplayer cannot set custom HTTP headers on a plain URL).
 * play_session_id MUST be included and unique per playback attempt — without
 * it, Jellyfin can serve back a stale cached transcode from an earlier
 * request instead of honoring the current profile/resolution (confirmed
 * against a real server: omitting it silently served an old 720x300 file
 * regardless of the maxWidth/maxHeight passed here).
 * burn_in_sub_index: -1 for none (the normal case — subtitles are rendered
 * client-side by mplayer, see jf_download_subtitle), otherwise a
 * JfSubtitle.index for an image-based track (pgssub/dvdsub/...) that has no
 * text to hand back as .srt and so can only be shown by having Jellyfin
 * burn it into the video itself. Confirmed on a real server that burning
 * subtitles into a mid-file restart forces Jellyfin to decode+discard from
 * the true start of the file up to the seek target (a seek to 40:00 dropped
 * exactly 40:00 worth of frames, on both mpeg2video and h264) — a
 * multi-minute stall for a deep seek, and nothing a client request
 * parameter can avoid. Only worth paying for image-based tracks, which have
 * no other way to display at all. */
int jf_stream_url(const JfConfig *cfg, const char *item_id,
                   const JfStreamProfile *profile, int64_t start_ticks,
                   const char *play_session_id, int burn_in_sub_index,
                   char *out, int outlen);

/* Builds a direct-play audio stream URL (static=true — no server transcode:
 * confirmed against a real server that a plain FLAC/MP3 track streams back
 * byte-identical with static=true, and this mplayer build already decodes
 * FLAC/MP3 fine, so there's no reason to pay for a transcode like video
 * needs). play_session_id isn't strictly required for direct play the way
 * it is for jf_stream_url()'s transcode (no cache to go stale), but it's
 * included anyway so progress reporting has a consistent session id. */
int jf_audio_stream_url(const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, char *out, int outlen);

/* Downloads subtitle track sub_index (JfSubtitle.index, i.e. the
 * MediaStreams[].Index) as .srt to dest_path — served directly (no
 * transcode) since Jellyfin can just hand back the original text file for
 * text-based subtitle codecs. media_source_id is the same as item_id for
 * every case this app handles (single-version items). mplayer loads/renders
 * this itself at runtime (slave "sub_load" + "sub_select") — no server
 * restart needed to change subtitles, unlike the old burn-in approach. */
int jf_download_subtitle(const JfConfig *cfg, const char *item_id,
                          const char *media_source_id, int sub_index,
                          const char *dest_path);

/* True for subtitle codecs Jellyfin can hand back as plain text (.srt) —
 * false for image-based codecs (pgssub/dvdsub/dvbsub/...) that only exist as
 * bitmaps and so need burn_in_sub_index in jf_stream_url instead. */
int jf_subtitle_is_text(const char *codec);

/* Playback progress reporting (Sessions/Playing family). play_session_id is
 * a client-generated opaque string reused across start/progress/stopped for
 * a single playback so the server can correlate/cancel the transcode job. */
void jf_report_start   (const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks);
void jf_report_progress(const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks, int paused);
void jf_report_stopped (const JfConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks);

/* Generates a client-side play session id, e.g. "misterfin-<pid>-<time>". */
void jf_make_play_session_id(char *out, int outlen);
