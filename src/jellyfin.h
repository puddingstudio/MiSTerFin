#pragma once
#include <stdint.h>

/* Jellyfin REST client — curl shelled out via popen(), no libcurl dependency
 * (matches MiSTerDVD's approach). All endpoints/params below were verified
 * against the official Jellyfin OpenAPI spec (see plan doc); a few optional
 * fields (MediaSourceId, PlaybackInfo negotiation) are intentionally omitted
 * rather than guessed — see README "Known limitations". */

#define JF_MAX_ITEMS   256
/* How many items one browse request asks for. Smaller than JF_MAX_ITEMS on
 * purpose: the whole response has to fit in jf_get's fixed buffer, and at
 * roughly 700 bytes of JSON per row a full 256 would land uncomfortably
 * close to it. Anything past a truncation point is silently lost (the parser
 * just stops at the first malformed object), so the margin matters more than
 * the round-trip count — and a library only pays for the pages actually
 * scrolled to. The client keeps ONE such page loaded at a time and slides it
 * (see jf_list_items' start_index). */
#define JF_PAGE_SIZE   128
/* Upper bound applied to a server-reported TotalRecordCount before it reaches
 * the client's window arithmetic. Far beyond any real library, but finite:
 * the alternative is trusting a remote integer not to overflow int math. */
#define JF_MAX_TOTAL_ITEMS 100000000
#define JF_ID_LEN      40
#define JF_NAME_LEN    256
#define JF_OVERVIEW_LEN 1024
#define JF_PERSON_NAME_LEN 64
#define JF_MAX_CAST    8
#define JF_MAX_SUBS    8
#define JF_MAX_AUDIO   8
/* Long enough for a fully-composed DisplayTitle — "English - Hearing
 * Impaired - Default - SUBRIP - External" is 55 characters. Storing the whole
 * thing means any shortening happens at draw time, where it's clipped to the
 * actual pixel width available, rather than being cut blindly here. */
#define JF_SUB_LABEL_LEN 72

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
    char label[JF_SUB_LABEL_LEN];       /* DisplayTitle if set, else Language. The server
                                         * builds DisplayTitle as the track's own Title
                                         * followed by whichever of Hearing Impaired /
                                         * Default / Forced / codec / External apply —
                                         * which is what separates a dialogue track from a
                                         * signs-and-songs one when both are tagged "eng" */
    char codec[16];                    /* MediaStreams[].Codec, e.g. "subrip", "ass",
                                         * "pgssub", "dvdsub" — see jf_subtitle_is_text() */
    int  is_forced;                    /* MediaStreams[].IsForced */
    int  is_default;                   /* MediaStreams[].IsDefault */
} JfSubtitle;

typedef struct {
    int  index;                        /* MediaStreams[].Index — pass as audioStreamIndex */
    char label[JF_SUB_LABEL_LEN];       /* DisplayTitle if set, else Language — the server
                                         * composes DisplayTitle as e.g. "English - Dolby
                                         * Digital 5.1 - Default", which is what actually
                                         * distinguishes a commentary from the main mix
                                         * when both are tagged the same language */
    char codec[16];                    /* "ac3", "aac", "dts", ... */
    int  channels;                     /* 2, 6, ... — 0 if unreported */
    int  is_default;                   /* MediaStreams[].IsDefault */
} JfAudioTrack;

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
    /* SeriesName — episodes only, empty otherwise. Redundant inside a season's
     * own episode list (the frame title already says which show it is), but
     * essential for the Continue Watching / Next Up rows, where an episode
     * called "Episode 03" is otherwise unidentifiable. */
    char       series_name[JF_NAME_LEN];
    char       collection_type[16];  /* CollectionType — "movies"/"tvshows"/"music"/...,
                                       * library views only (jf_list_views), empty otherwise */
    JfItemType type;
    /* Non-zero only for the client's own synthetic home-screen entries (see
     * JF_SYNTH_*). parse_item_fields memsets, so anything that came from the
     * server always reads 0 — which is the point: identifying these by id
     * alone let a hostile server return an item with that id and shadow a
     * real library into being unreachable. */
    int        synthetic;
    int64_t    runtime_ticks;          /* RunTimeTicks, 0 if unknown */
    int        child_count;            /* ChildCount — track count for a MusicAlbum
                                         * (jf_list_items only requests this field);
                                         * 0/unset for every other item type. */
    /* RecursiveItemCount — every non-folder, non-virtual descendant, i.e.
     * the episode count for a Series (confirmed against the server's own
     * count query, which filters exactly that way, so unaired/missing
     * episodes are excluded — what you'd actually want to read on a row).
     * Requires Fields=RecursiveItemCount AND EnableUserData=true: the
     * server computes it inside its user-data block, batched across the
     * whole result set in one query. That batching is the entire point of
     * using it — the client used to issue one extra recursive count request
     * PER SERIES ROW to get this same number, so a 200-series library meant
     * 200 sequential curl invocations before the list could draw. 0 if not
     * requested or not applicable. */
    int        recursive_item_count;
    int64_t    resume_ticks;           /* UserData.PlaybackPositionTicks */
    int        played;                 /* UserData.Played */
    int        index_number;           /* episode/season number, -1 if n/a */
    int        parent_index_number;    /* ParentIndexNumber — season number for an
                                         * episode, -1 if n/a. With index_number this
                                         * gives the "S1E03" an out-of-context episode
                                         * row needs. */
    double     community_rating;       /* CommunityRating, e.g. 7.4 — 0 if unset/not fetched */
    JfPerson   cast[JF_MAX_CAST];       /* Actors only, empty unless fetched via jf_get_item_details */
    int        cast_count;
    JfSubtitle subs[JF_MAX_SUBS];       /* empty unless fetched via jf_get_item_details */
    int        sub_count;
    /* Selectable audio tracks, same lifecycle as subs[]. Unlike subtitles
     * these can't be switched client-side: the server transcodes exactly one
     * audio stream into the delivered container, so changing tracks means
     * rebuilding the stream URL and restarting playback. */
    JfAudioTrack audio[JF_MAX_AUDIO];
    int          audio_count;
    /* Source (original file) video specs — from MediaStreams, empty unless
     * fetched via jf_get_item_details. What's actually streamed to the
     * device is different (see JfStreamProfile) — this is for display only. */
    char       source_video_codec[16];
    int        source_width, source_height;
    int64_t    source_bitrate;          /* bits/sec, 0 if unknown */
    /* Display aspect ratio (width/height), from the video stream's own
     * "AspectRatio" field (e.g. "16:9") rather than source_width/height —
     * those are raw pixel dims and can be wrong for anamorphic sources
     * (e.g. a PAL DVD's 720x576 is really 16:9 despite a 5:4 pixel ratio,
     * via non-square SAR). 0 if not fetched/parseable — caller should fall
     * back to source_width/source_height in that case. */
    double     source_aspect;
} JfItem;

typedef struct {
    char server[256];   /* e.g. http://192.168.2.10:8096 (no trailing slash) */
    char api_key[64];   /* config line 2, may be empty — see jf_config_load */
    char username[64];  /* config line 3 — resolved to user_id via jf_resolve_user_id() */
    char user_id[JF_ID_LEN];   /* empty until resolved; everything else needs this, not username */
    char tv_mode[8];     /* "PAL" (default) or "NTSC" — 4th, optional config line */
    /* The credential actually sent with every request, as both the
     * Authorization header's Token and the ApiKey query parameter on media
     * URLs. Either an API key copied from api_key, or an access token earned
     * through Quick Connect — the server treats the two identically, so
     * nothing downstream needs to know which it got. Empty means
     * unauthenticated, which is valid for exactly one thing: starting a Quick
     * Connect request. */
    char token[192];
    /* Identifies this install to the server. Generated once and persisted —
     * NOT a compile-time constant shared by every copy of the app, which it
     * used to be.
     *
     * That mattered the moment Quick Connect arrived. Jellyfin's
     * GetAuthorizationToken logs out every existing session matching
     * (DeviceId, UserId) whenever a new one authenticates, so two MiSTers
     * signed in as the same user and reporting the same DeviceId would
     * silently revoke each other's token on every launch — and since a
     * revoked token sends this client back through Quick Connect, they'd
     * take turns kicking one another out indefinitely. The API key path
     * creates no session, which is why this was harmless before. */
    char device_id[64];
    /* Transcode resolution/bitrate requested for video, overridable from the
     * config file (see jf_config_load). Configurable because the useful
     * resolution ceiling on this hardware is an open question that can only
     * be answered by measuring on a real MiSTer — and rebuilding and
     * reflashing for each data point makes that experiment tedious enough
     * that it doesn't get done. */
    int  profile_width;
    int  profile_height;
    int  profile_bitrate;
} JfConfig;

/* One in-flight Quick Connect request. */
typedef struct {
    char secret[128];   /* the client's half — never shown to the user */
    char code[16];      /* the short code the user types into Jellyfin */
} JfQuickConnect;

/* Folds a UTF-8 string down to the ASCII subset the on-screen bitmap font can
 * draw (accents stripped, smart quotes and dashes normalised, anything with
 * no ASCII form collapsed to a single '?'). Every server string that reaches
 * the screen already goes through this on the way into a JfItem; it's exposed
 * for callers that get text from somewhere else, e.g. downloaded subtitles. */
void jf_text_to_display(const char *utf8, char *out, int outlen);

/* Loads /media/fat/misterfin/jellyfin.conf (falls back to ./jellyfin.conf for
 * desktop testing). 4 lines: server, api_key, username, tv_mode (optional,
 * defaults to PAL). Returns 1 if there's at least a server URL, 0 otherwise.
 *
 * Only the server URL is genuinely required now. An empty api_key/username is
 * no longer a failure — it means "authenticate with Quick Connect instead",
 * which is the point: obtaining an API key needs the admin dashboard, so
 * requiring one made every user an admin of their own server or dependent on
 * someone who is. cfg->user_id is NOT populated here; see jf_resolve_user_id
 * or jf_quick_connect_authenticate. */
int jf_config_load(JfConfig *cfg);

/* Whether cfg carries a usable credential at all (an API key from the config
 * file, or a token loaded/earned at runtime). */
int jf_has_credential(const JfConfig *cfg);

/* Fills in cfg->device_id, generating and persisting a fresh random GUID the
 * first time. Call once after jf_config_load, before any request — the id
 * goes in the Authorization header of every one of them, and must not change
 * between authenticating and using the resulting token. Losing the file just
 * means the server sees a new device; nothing breaks. */
void jf_device_id_init(JfConfig *cfg);

/* ── Quick Connect ─────────────────────────────────────────────────────────
 * Authenticates without ever handling a password or an admin-issued API key:
 * the client asks the server to start a request, shows the user a short code,
 * and the user approves it from an already-signed-in Jellyfin session. What
 * comes back is an ordinary user access token.
 *
 * The resulting token is a narrower credential than the API key path it
 * replaces. An API key authenticates as the server, not as a person: which
 * account gets its watch state updated is decided by the userId this client
 * chooses to send (resolved from the configured username), and the same key
 * could just as well address any other account. It's also why progress has to
 * be reported per-user rather than through the session endpoint — see
 * report_user_data. A Quick Connect token is the user, so neither applies. */

/* 1 if the server has Quick Connect switched on, 0 if not or unreachable. */
int jf_quick_connect_enabled(const JfConfig *cfg);

/* Starts a request and fills in the secret + the code to show the user.
 * Returns 1 on success. */
int jf_quick_connect_initiate(const JfConfig *cfg, JfQuickConnect *qc);

/* Polls a pending request: 1 = approved, 0 = still waiting, -1 = the request
 * is gone (expired, denied, or the server forgot it). Callers should poll at
 * a human pace — a few seconds — since it's a person walking to another
 * device that this is waiting on. */
int jf_quick_connect_poll(const JfConfig *cfg, const JfQuickConnect *qc);

/* Exchanges an approved request for an access token, filling in cfg->token
 * and cfg->user_id (and cfg->username, from whoever approved it). Returns 1
 * on success. */
int jf_quick_connect_authenticate(JfConfig *cfg, const JfQuickConnect *qc);

/* Persist / restore the earned token so Quick Connect is a one-time step
 * rather than something to repeat at every launch. Stored next to
 * jellyfin.conf as plain text — no worse than the API key that already lives
 * there, and anyone who can read the SD card can read both. */
int jf_token_save(const JfConfig *cfg);
int jf_token_load(JfConfig *cfg);
void jf_token_clear(void);

/* Cheap authenticated request, to tell a still-valid saved token from one the
 * server has since revoked. Returns 1 if the credential works. */
int jf_credential_works(const JfConfig *cfg);

/* Looks up cfg->username in GET /Users and fills in cfg->user_id. The raw
 * Jellyfin user id (a GUID) is not something a user can easily find in the
 * Jellyfin web UI (it only shows up in a dashboard URL) — a plain username
 * is what everyone actually knows, so the client resolves it instead of
 * asking for the id directly.
 * Returns 1 on success, 0 if the server answered but no user matched
 * cfg->username (typo, or the user genuinely doesn't exist), or -1 if the
 * request itself failed (server unreachable — wrong URL, server down,
 * network issue) — distinct from 0 so the setup screen can tell "can't
 * reach your server" apart from "server's fine, but check your config"
 * instead of always blaming the config either way. */
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

/* Direct children of parent_id (a view, a folder, ...), starting at
 * start_index — one window of a potentially much longer list. Writes the
 * server's untruncated TotalRecordCount to *total_out (may be NULL), which
 * is what lets the caller know there's more to page to; without it a client
 * cannot tell "that's the whole library" from "that's all that fit".
 *
 * Returns the number of items, or -1 if the request itself failed. That's a
 * distinct value from 0 on purpose: a caller sliding a window has to roll
 * back to the page it already had rather than commit an empty one, and an
 * empty page is a legitimate answer it must not confuse with a dead network. */
int jf_list_items(const JfConfig *cfg, const char *parent_id, int start_index,
                   JfItem *out, int max, int64_t *total_out);

/* Same shape as jf_list_items but recursive, and filtered to item_type if
 * non-NULL/non-empty (a BaseItemKind string, e.g. "MusicAlbum"). Used by
 * the carousel's background cover grid to reach real leaf-level art (movie/
 * series/album covers) even for libraries organized in intermediate folders
 * — a by-artist music library's direct children are MusicArtist folders,
 * which often carry no cover art of their own; a plain non-recursive
 * listing there found nothing to show (confirmed against a real server). */
int jf_list_items_recursive(const JfConfig *cfg, const char *parent_id,
                             const char *item_type, JfItem *out, int max);

/* Random batch of Audio items anywhere under parent_id (e.g. a whole Music
 * library) — SELECT on the artist list starts an infinite shuffle with
 * this, refetching a fresh batch each time the current one runs out. */
int jf_list_random_tracks(const JfConfig *cfg, const char *parent_id, JfItem *out, int max);

/* Sentinel ids for the two synthetic home-screen entries. These aren't real
 * library views — there is nothing on the server they correspond to — so they
 * get ids that cannot collide with a Jellyfin GUID while still surviving
 * jf_sanitize_id unchanged (which is what makes them safe as grid-cache
 * filenames). */
#define JF_VIEW_RESUME  "misterfin-resume"
#define JF_VIEW_NEXTUP  "misterfin-nextup"

/* Values for JfItem.synthetic. The ids above remain as grid-cache keys; these
 * are what the code actually branches on. */
#define JF_SYNTH_RESUME 1
#define JF_SYNTH_NEXTUP 2

/* Partly-watched items across the whole library, most recently played first
 * (Jellyfin's "Continue Watching"). Returns movies and episodes together.
 * Writes the untruncated count to *total_out (may be NULL) so the home screen
 * can show one without fetching the whole row. */
int jf_list_resume(const JfConfig *cfg, JfItem *out, int max, int64_t *total_out);

/* The next unwatched episode of each series in progress (Jellyfin's
 * "Next Up"). Episodes only, by definition. */
int jf_list_nextup(const JfConfig *cfg, JfItem *out, int max, int64_t *total_out);

/* TV hierarchy. */
int jf_list_seasons(const JfConfig *cfg, const char *series_id, JfItem *out, int max);
/* Paginated like jf_list_items — a season is normally short, but "all
 * episodes of a long-running series" in one folder is not, and it costs
 * nothing to treat both the same way. */
int jf_list_episodes(const JfConfig *cfg, const char *series_id, const char *season_id,
                      int start_index, JfItem *out, int max, int64_t *total_out);

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

/* What the client asks the server to transcode down to, before mplayer scales
 * it back up to fill the framebuffer. Deliberately small: this device decodes
 * in scalar C with no NEON path, and total pixel count is what costs CPU —
 * confirmed on hardware that bitrate barely matters (2Mbps and 8Mbps both sat
 * at ~34% utime with no A/V drift) while 720x576 pegged the CPU and drifted
 * continuously. */
#define JF_PROFILE_DEFAULT_W    480
#define JF_PROFILE_DEFAULT_H    270
#define JF_PROFILE_DEFAULT_RATE 8000000

/* Bounds for a profile read from the config file. Wide enough not to get in
 * the way of experimenting, narrow enough that a typo can't ask the server
 * for something absurd. */
#define JF_PROFILE_MIN_W     160
#define JF_PROFILE_MAX_W    1920
#define JF_PROFILE_MIN_H     120
#define JF_PROFILE_MAX_H    1080
#define JF_PROFILE_MIN_RATE   100000
#define JF_PROFILE_MAX_RATE 50000000

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
 * no other way to display at all.
 * audio_stream_index: a JfAudioTrack.index to select a specific audio
 * track, or -1 to let the server pick the source's default. Audio is
 * transcoded server-side into the delivered container, so unlike a text
 * subtitle this cannot be switched client-side — changing it means building
 * a new URL and restarting playback at the current position. */
int jf_stream_url(const JfConfig *cfg, const char *item_id,
                   const JfStreamProfile *profile, int64_t start_ticks,
                   const char *play_session_id, int burn_in_sub_index,
                   int audio_stream_index,
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
                         const char *play_session_id, int64_t position_ticks,
                         int played);

/* Generates a client-side play session id, e.g. "misterfin-<pid>-<time>". */
void jf_make_play_session_id(char *out, int outlen);
