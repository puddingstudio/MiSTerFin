#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "fb.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#include "font8x8.h"
#include "font_vcr16x16.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ddr.h"
#include "jellyfin.h"

#define MPLAYER      "/media/fat/misterfin/mplayer-arm"
#define POSTER_TMP   "/tmp/misterfin_poster.img"
#define CRASH_LOG    "/media/fat/misterfin/crash.log"
/* mplayer's -af export writes live PCM samples to this mmap'd file for the
 * now-playing visualizer to read — confirmed supported on this build
 * (some builds/filters aren't, see the mplayer/subfont -osdlevel comments
 * elsewhere in this file for other examples of that). Unlinked before each
 * track starts so a stale file from a previous track can't be misread as
 * current audio for the first frame or two. */
#define AF_EXPORT_PATH "/tmp/misterfin_af_export"
/* Hardcoded inside the shared mplayer-arm binary's patched vo_fbdev.c (built
 * for MiSTerDVD, reused here unchanged) — NOT ours to rename. Its presence
 * makes vo_fbdev wait for vblank before each frame, trading a little extra
 * decode latency for tear-free output. */
#define VSYNC_FLAG   "/tmp/misterdvd_vsync"

#define VISIBLE      7   /* was 6 — list had a lot of unused space above the hint row, see draw_browse's bottom fade */
#define ROW_H        30
#define SAFE_X       24
#define SAFE_Y       20
#define SAFE_Y_BOT   SAFE_Y   /* match the top title margin — same overscan/visibility balance */
#define SEEK_STEP        30.0
#define AUDIO_SEEK_STEP  10.0   /* smaller than video's SEEK_STEP — tracks are short, real in-place seek (see STATE_PLAYING_AUDIO) */
#define SEEK_DEBOUNCE     0.6   /* seconds to wait for more presses before firing the seek */
#define FLASH_DURATION_SEC 1.2  /* how long a flash_message() stays pinned on screen, see g_flash_until */
#define PROGRESS_REPORT_INTERVAL 10.0
/* Passed to mplayer as "-delay" (see play()). */
#define AUDIO_DELAY_SEC 0.20
/* Subtitles run ahead of the g_play_offset-corrected clock by a further
 * fixed amount on top of AUDIO_DELAY_SEC — dialed in via the submenu's
 * live LEFT/RIGHT sync adjustment (g_sub_delay_extra) on real hardware and
 * confirmed to read spot-on at +1.4s beyond AUDIO_DELAY_SEC. Baked in here
 * as the new default so a subtitle is in sync out of the box; the live
 * adjustment still exists for whatever this constant doesn't fully cover
 * (e.g. a subtitle file with its own internal timing drift). */
#define SUBTITLE_SYNC_FUDGE_SEC 1.4

/* ── colour palette ──────────────────────────────────────────────────────── */
#define COL_TITLE   0xFF,0xE0,0x40
#define COL_ITEM    0xCC,0xCC,0xCC
#define COL_DIM     0x80,0x80,0x80
#define COL_SEL_BG  0x10,0x40,0x90
#define COL_SEL_FG  0xFF,0xFF,0xFF
#define COL_HINT    0x80,0x80,0x80
#define COL_WATCHED 0x40,0xCC,0x40
#define COL_RESUME  0xFF,0xC0,0x40
#define COL_ERR     0xFF,0x60,0x60

static volatile int g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

static char g_setup_reason[64];   /* set once at startup, redrawn every frame by draw_setup_screen() */

static void cursor_show(void);  /* forward decl for emergency_cleanup */

static void emergency_cleanup(void)
{
    ddr_close();
    cursor_show();
}

static void on_fatal(int s)
{
    int lfd = open(CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) {
        const char *pre = "signal=";
        write(lfd, pre, 7);
        char d[4]; int n = 0; int sv = s;
        if (sv >= 100) d[n++] = '0' + sv/100;
        if (sv >= 10)  d[n++] = '0' + (sv/10)%10;
        d[n++] = '0' + sv%10;
        d[n++] = '\n';
        write(lfd, d, n);
        close(lfd);
    }
    emergency_cleanup();
    signal(s, SIG_DFL);
    raise(s);
}

/* ── update check (About screen) — same pattern as MiSTerDVD ─────────────── */

typedef enum { UPD_CHECKING, UPD_OK, UPD_AVAILABLE, UPD_FAILED } UpdateState;

static UpdateState     g_upd_state = UPD_CHECKING;
static char            g_upd_latest[32] = {0};
static pthread_mutex_t g_upd_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Checks github.com/puddingstudio/MiSTerFin's latest release tag against
 * APP_VERSION. No-op-safe if that repo doesn't exist yet (this project is
 * local-only for now) — the request just fails and the About screen shows
 * no update line, same as any other network hiccup. */
static void *update_check_thread(void *arg)
{
    (void)arg;
    FILE *f = popen(
        "curl -sfk --max-time 8 "
        "https://api.github.com/repos/puddingstudio/MiSTerFin/releases/latest "
        "2>/dev/null", "r");
    if (!f) {
        pthread_mutex_lock(&g_upd_mutex);
        g_upd_state = UPD_FAILED;
        pthread_mutex_unlock(&g_upd_mutex);
        return NULL;
    }
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);

    char *p = strstr(buf, "\"tag_name\"");
    if (!p) goto fail;
    p = strchr(p, ':'); if (!p) goto fail;
    p++;
    while (*p == ' ' || *p == '"') p++;
    char tag[32] = {0};
    int i = 0;
    while (*p && *p != '"' && i < 31) tag[i++] = *p++;

    pthread_mutex_lock(&g_upd_mutex);
    strncpy(g_upd_latest, tag, sizeof(g_upd_latest) - 1);
    g_upd_state = (strcmp(tag, APP_VERSION) == 0) ? UPD_OK : UPD_AVAILABLE;
    pthread_mutex_unlock(&g_upd_mutex);
    return NULL;

fail:
    pthread_mutex_lock(&g_upd_mutex);
    g_upd_state = UPD_FAILED;
    pthread_mutex_unlock(&g_upd_mutex);
    return NULL;
}

static void cursor_hide(void)
{
    const char *ttys[] = { "/dev/tty0", "/dev/tty1", "/dev/tty", "/dev/console", NULL };
    for (int i = 0; ttys[i]; i++) {
        int fd = open(ttys[i], O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, KDSETMODE, KD_GRAPHICS);
        write(fd, "\033[?25l", 6);
        write(fd, "\033[2J",   4);
        close(fd);
    }
    int sf = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY);
    if (sf >= 0) { write(sf, "0", 1); close(sf); }
}

static void cursor_show(void)
{
    const char *ttys[] = { "/dev/tty0", "/dev/tty1", "/dev/tty", "/dev/console", NULL };
    for (int i = 0; ttys[i]; i++) {
        int fd = open(ttys[i], O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, KDSETMODE, KD_TEXT);
        write(fd, "\033[?25h", 6);
        close(fd);
    }
    int sf = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY);
    if (sf >= 0) { write(sf, "1", 1); close(sf); }
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fmt_time(char *buf, size_t sz, double secs)
{
    if (secs < 0) secs = 0;
    int s = (int)secs;
    int h = s / 3600; s %= 3600;
    int m = s / 60;   s %= 60;
    if (h > 0) snprintf(buf, sz, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, sz, "%d:%02d", m, s);
}

/* ── text rendering ──────────────────────────────────────────────────────── */

/* VCR OSD Mono (see tools/rasterize_vcr_font.c) rendered natively at 16x16
 * for scale=2 text — tried and, on user comparison against the original
 * hand-drawn font8x8_basic (blown up 2x), the original was preferred. Kept
 * disabled rather than deleted in case it's wanted later — flip this to 1
 * to re-enable, no other code changes needed. */
#define UI_USE_VCR_FONT 0

static void draw_char(FBDev *fb, int x, int y, unsigned char c, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    if (UI_USE_VCR_FONT && scale == 2) {
        unsigned char cc = (c < 0x20 || c > 0x7E) ? '?' : c;
        const uint8_t (*glyph16)[16] = font_vcr16x16[cc - 0x20];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 16; col++) {
                uint8_t a = glyph16[row][col];
                if (a) fb_fill_rect_alpha(fb, x + col, y + row, 1, 1, r, g, b, a);
            }
        return;
    }

    if (c >= 128) c = '?';
    const uint8_t *glyph = font8x8_basic[c];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((bits >> col) & 1)
                fb_fill_rect_alpha(fb, x + col*scale, y + row*scale,
                                   scale, scale, r, g, b, 255);
        }
    }
}

static void draw_text(FBDev *fb, int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (; *s; s++, x += 8*scale)
        draw_char(fb, x, y, (unsigned char)*s, scale, r, g, b);
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

/* Like draw_text, but skips any character whose whole glyph cell doesn't
 * fit within [clip_x0, clip_x1) — used for the scrolling browse title
 * marquee so it doesn't run into the clock/spinner area to its right (or
 * off the left edge while scrolling). Character-granularity, not
 * pixel-perfect, but fine for a continuously-moving marquee. */
static void draw_text_clipped(FBDev *fb, int x, int y, const char *s, int scale,
                               uint8_t r, uint8_t g, uint8_t b, int clip_x0, int clip_x1)
{
    for (; *s; s++, x += 8 * scale)
        if (x >= clip_x0 && x + 8 * scale <= clip_x1)
            draw_char(fb, x, y, (unsigned char)*s, scale, r, g, b);
}

/* Truncates s in-place (with a trailing "...") so it fits within max_w
 * pixels at the given scale — draw_text itself doesn't clip, so a long
 * title would otherwise run into whatever's to its right. */
static void truncate_to_width(char *s, int scale, int max_w)
{
    int max_chars = max_w / (8 * scale);
    int len = (int)strlen(s);
    if (len <= max_chars) return;
    if (max_chars > 3) { s[max_chars - 3] = '\0'; strcat(s, "..."); }
    else if (max_chars > 0) s[max_chars] = '\0';
}

static int draw_wrapped(FBDev *fb, int x, int y, const char *text,
                         int scale, int max_w, int max_lines,
                         uint8_t r, uint8_t g, uint8_t b)
{
    int cols    = max_w / (8 * scale);
    int line_h  = 8 * scale + 2;
    char line[256] = {0};
    int  li         = 0;
    int  drawn      = 0;
    const char *p   = text;

    while (*p && drawn < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *we = p;
        while (*we && *we != ' ') we++;
        int wlen = (int)(we - p);
        if (li > 0 && li + 1 + wlen > cols) {
            int is_last = (drawn == max_lines - 1);
            if (is_last && *we) {
                if (li > cols - 3) li = cols - 3;
                strcpy(line + li, "...");
            }
            draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
            drawn++; li = 0; memset(line, 0, sizeof(line));
        }
        if (li > 0 && li < (int)sizeof(line) - 2) line[li++] = ' ';
        if (li + wlen < (int)sizeof(line) - 1) { memcpy(line+li, p, wlen); li += wlen; }
        p = we;
    }
    if (li > 0 && drawn < max_lines) {
        draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
        drawn++;
    }
    return drawn * line_h;
}

/* ── input (evdev gamepad, same model as MiSTerDVD) ─────────────────────── */

#define MAX_INPUT_FDS 8
static int input_fds[MAX_INPUT_FDS];
static int input_swap_ab[MAX_INPUT_FDS];
static int input_is_virtual[MAX_INPUT_FDS];
static int input_count = 0;

/* Some 8BitDo SNES-style pads (confirmed on the SFC30 via raw evdev capture)
 * report their printed A/B buttons as BTN_SOUTH/BTN_EAST swapped relative to
 * the usual position-based convention (printed A -> BTN_SOUTH, printed B ->
 * BTN_EAST) — the firmware enumerates buttons in legacy SNES ordinal order
 * rather than by physical/compass position, unlike XInput-style pads. Swap
 * them back per-device so A is always "confirm" and B is always "back". */
static int device_needs_ab_swap(const char *name)
{
    return strstr(name, "SFC30") != NULL;
}

/* MiSTer's own OSD layer echoes every physical joystick press as a
 * synthetic keyboard event on a separate virtual device (confirmed via raw
 * evdev capture: pressing a gamepad button also fires an unrelated KEY_*
 * code on this device, per whatever key MiSTer's own default joystick-to-
 * OSD table happens to assign it). Turns out the SFC30's D-pad specifically
 * only ever arrives THROUGH this echo (as KEY_UP/DOWN/LEFT/RIGHT — it has
 * no EV_ABS capability of its own, confirmed via /proc/bus/input/devices),
 * so it can't just be closed outright. Instead only arrow-key codes from it
 * are trusted (see input_poll) — action keys (Enter/Esc/Space/...) are
 * dropped since those collide with keys we bind for real keyboards/pads. */
static int device_is_mister_virtual(const char *name)
{
    return strcmp(name, "MiSTer virtual input") == 0;
}

#define INP_UP     0x01
#define INP_DOWN   0x02
#define INP_A      0x04
#define INP_B      0x08
#define INP_LEFT   0x10
#define INP_RIGHT  0x20
#define INP_START  0x40
#define INP_SELECT 0x80
#define INP_L      0x100
#define INP_R      0x200

static void input_open(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && input_count < MAX_INPUT_FDS) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        input_swap_ab[input_count]    = device_needs_ab_swap(name);
        input_is_virtual[input_count] = device_is_mister_virtual(name);
        input_fds[input_count++] = fd;
    }
    closedir(d);
}

static void input_drain(void)
{
    struct input_event ev;
    for (int i = 0; i < input_count; i++)
        while (read(input_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {}
}

static void input_close(void)
{
    for (int i = 0; i < input_count; i++) close(input_fds[i]);
    input_count = 0;
}

static int input_poll(void)
{
    struct input_event ev;
    int mask = 0;
    for (int i = 0; i < input_count; i++) {
        while (read(input_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                int code = ev.code;
                if (input_swap_ab[i]) {
                    if      (code == BTN_SOUTH) code = BTN_EAST;
                    else if (code == BTN_EAST)  code = BTN_SOUTH;
                }
                /* MiSTer's own core process exclusively grabs directly-wired
                 * USB joysticks for FPGA/OSD routing (confirmed via
                 * /proc/PID/fd: the "MiSTer" process holds the wired
                 * SFC30's event node open, and no other reader ever sees
                 * its raw events) so the virtual echo device is the ONLY
                 * input path for a wired pad, meaning its confirm/cancel/
                 * nav keys must stay trusted here. Action keys we bind
                 * ourselves for a real keyboard (Space/Tab/PageUp/PageDown)
                 * are still dropped from it — those aren't part of MiSTer's
                 * own OSD table and only ever showed up as an arbitrary,
                 * colliding echo. */
                if (input_is_virtual[i] &&
                    code != KEY_UP && code != KEY_DOWN &&
                    code != KEY_LEFT && code != KEY_RIGHT &&
                    code != KEY_ENTER && code != KEY_ESC && code != KEY_BACK) {
                    continue;
                }
                switch (code) {
                case BTN_EAST:               mask |= INP_A;      break;
                case BTN_SOUTH:              mask |= INP_B;      break;
                case KEY_ENTER:              mask |= INP_A;      break;
                case KEY_ESC:
                case KEY_BACK:               mask |= INP_B;      break;
                case BTN_START: case KEY_PAUSE: case KEY_HOME: mask |= INP_START;  break;
                case BTN_SELECT: case KEY_TAB:  mask |= INP_SELECT; break;
                case KEY_UP:                     mask |= INP_UP;    break;
                case KEY_DOWN:                   mask |= INP_DOWN;  break;
                case KEY_LEFT:                   mask |= INP_LEFT;  break;
                case KEY_RIGHT:                  mask |= INP_RIGHT; break;
                case BTN_TL: case KEY_PAGEUP:    mask |= INP_L;     break;
                case BTN_TR: case KEY_PAGEDOWN:  mask |= INP_R;     break;
                }
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0Y) {
                    if (ev.value == -1) mask |= INP_UP;
                    if (ev.value ==  1) mask |= INP_DOWN;
                }
                if (ev.code == ABS_HAT0X) {
                    if (ev.value == -1) mask |= INP_LEFT;
                    if (ev.value ==  1) mask |= INP_RIGHT;
                }
            }
        }
    }
    return mask;
}

/* ── app state ────────────────────────────────────────────────────────────── */

typedef enum { STATE_CONFIG_ERROR, STATE_BROWSE, STATE_INFO, STATE_PLAYING, STATE_PLAYING_AUDIO } AppState;
typedef enum { FRAME_VIEWS, FRAME_ITEMS, FRAME_SEASONS, FRAME_EPISODES } FrameKind;

#define MAX_STACK 8

typedef struct {
    FrameKind kind;
    char      title[128];
    char      parent_id[JF_ID_LEN];  /* FRAME_ITEMS */
    char      series_id[JF_ID_LEN];  /* FRAME_SEASONS / FRAME_EPISODES */
    char      season_id[JF_ID_LEN];  /* FRAME_EPISODES */
} BrowseFrame;

static JfConfig   g_cfg;
static BrowseFrame g_stack[MAX_STACK];
static int         g_stack_depth = 0;

static JfItem g_items[JF_MAX_ITEMS];
static int    g_item_count = 0;
static int    g_sel = 0, g_scroll = 0;
static double g_marquee_px = 0.0;         /* scroll offset for an over-long title, see draw_browse */
static char   g_marquee_title[128] = "";  /* last title drawn — reset the offset when it changes */
/* Per-library item counts for the root carousel (see draw_browse_carousel),
 * parallel to g_items[] and only meaningful while the root FRAME_VIEWS frame
 * is loaded — fetched once in fetch_frame() alongside the views themselves
 * (3 small Limit=0 count requests for this user's library, one per view).
 * -1 = fetch failed, caller just omits the count line for that card. */
static int64_t g_view_counts[JF_MAX_ITEMS];
/* Episode count per row, parallel to g_items[], only meaningful for
 * JF_TYPE_SERIES rows in a FRAME_ITEMS listing (a TV library's series
 * list) — fetched alongside the listing itself in fetch_frame(). Season
 * count needs no such array: it's it->child_count, already free on the
 * same request (confirmed on a real server that a Series' ChildCount is
 * its season count, unlike a top-level library view's — see
 * jf_count_items's own comment for that distinction). -1 = fetch failed or
 * not a series, caller omits the episode part of the line. */
static int64_t g_series_episode_counts[JF_MAX_ITEMS];
/* Root screen (FRAME_VIEWS) rendering mode — 0 = carousel (default),
 * 1 = classic list, toggled by SELECT (see the STATE_BROWSE input handling
 * below). Persists for the whole app session, not just this one visit to
 * the root, same way any other user preference toggle would. */
static int    g_root_list_mode = 0;
/* Last-selected root library index — restored by fetch_frame() when
 * popping all the way back to the root screen, so it doesn't always reset
 * to the first library. Kept up to date by the STATE_BROWSE input handling
 * whenever g_sel changes while at the root. */
static int    g_root_sel = 0;
/* B at the root browse screen opens this instead of exiting immediately —
 * same "B opens, B cancels, A confirms" behavior as MiSTer-Toasty-Squadron's
 * own exit-confirm, added so a stray B press while browsing can't silently
 * quit the app. See the STATE_BROWSE input handling and draw_confirm_exit(). */
static int    g_confirm_exit = 0;
/* SELECT on the music library's artist list starts an infinite shuffle
 * instead of drilling in — reuses g_items/g_item_count/g_audio_queue_pos
 * exactly like a normal album's track list, just refilled with a fresh
 * random batch (see jf_list_random_tracks) whenever it runs out instead of
 * falling back to STATE_BROWSE. Cleared on stop, which also re-fetches the
 * artist frame since g_items was overwritten with the shuffle batch. */
static int    g_shuffle_mode = 0;
/* Now-playing background effect, cycled by SELECT (see the
 * STATE_PLAYING_AUDIO input handling) — 0 = starfield, 1 = rain,
 * 2 = Toasty Squadron sprites (see draw_toasty). Persists for the whole
 * app session, same as g_root_list_mode. */
#define NOW_PLAYING_BG_COUNT 3
static int    g_now_playing_bg = 0;
static const char *NOW_PLAYING_BG_NAMES[] = { "Starfield", "Rain", "Toasty Squadron" };
/* Label shows briefly on change then disappears, rather than sitting on
 * screen permanently — set to now_sec()+1.5 wherever g_now_playing_bg
 * changes (see the STATE_PLAYING_AUDIO SELECT handling), 0 = not shown. */
static double g_now_playing_bg_shown_until = 0.0;

static JfItem g_info_item;   /* item currently shown on the info screen */

static void draw_spinner_frame(FBDev *fb, int frame_idx);   /* forward decl — defined below, used by info_assets_load */

/* ── info-screen hero assets (backdrop, logo, cast photos) ───────────────── */

#define CAST_DISPLAY_MAX 5

static uint8_t *g_backdrop_px = NULL;
static int      g_backdrop_w = 0, g_backdrop_h = 0;
static uint8_t *g_logo_px = NULL;
static int      g_logo_w = 0, g_logo_h = 0;
static uint8_t *g_cast_px[CAST_DISPLAY_MAX];
static int      g_cast_px_w[CAST_DISPLAY_MAX], g_cast_px_h[CAST_DISPLAY_MAX];

static uint8_t *load_image_tmp(const char *tmp_path, int *w, int *h)
{
    int channels = 0;
    uint8_t *px = stbi_load(tmp_path, w, h, &channels, 4);
    unlink(tmp_path);
    return px;
}

static void info_assets_free(void)
{
    if (g_backdrop_px) { stbi_image_free(g_backdrop_px); g_backdrop_px = NULL; }
    if (g_logo_px)     { stbi_image_free(g_logo_px);     g_logo_px = NULL; }
    for (int i = 0; i < CAST_DISPLAY_MAX; i++)
        if (g_cast_px[i]) { stbi_image_free(g_cast_px[i]); g_cast_px[i] = NULL; }
    g_backdrop_w = g_backdrop_h = g_logo_w = g_logo_h = 0;
}

/* Fetches full item details (overview, cast, backdrop/logo tags — omitted
 * from browse-list rows to keep those cheap) plus the actual images, one
 * request at a time. Advances the loading spinner between requests so the
 * wait (several sequential downloads) has visible feedback. */
static void info_assets_load(FBDev *fb, const JfItem *list_item, int *spinner_frame)
{
    info_assets_free();

    if (!jf_get_item_details(&g_cfg, list_item->id, &g_info_item))
        g_info_item = *list_item;   /* degrade gracefully: keep the shallow row copy */

    draw_spinner_frame(fb, (*spinner_frame)++); fb_flip(fb);
    if (g_info_item.backdrop_tag[0]) {
        int dl_ok = jf_download_item_image(&g_cfg, g_info_item.backdrop_item_id, "Backdrop/0",
                                            g_info_item.backdrop_tag, 640, POSTER_TMP);
        if (dl_ok)
            g_backdrop_px = load_image_tmp(POSTER_TMP, &g_backdrop_w, &g_backdrop_h);
    }

    draw_spinner_frame(fb, (*spinner_frame)++); fb_flip(fb);
    if (g_info_item.logo_tag[0]) {
        int dl_ok = jf_download_item_image(&g_cfg, g_info_item.logo_item_id, "Logo",
                                            g_info_item.logo_tag, 400, POSTER_TMP);
        if (dl_ok)
            g_logo_px = load_image_tmp(POSTER_TMP, &g_logo_w, &g_logo_h);
    }

    int cast_n = g_info_item.cast_count;
    if (cast_n > CAST_DISPLAY_MAX) cast_n = CAST_DISPLAY_MAX;
    for (int i = 0; i < cast_n; i++) {
        draw_spinner_frame(fb, (*spinner_frame)++); fb_flip(fb);
        JfPerson *p = &g_info_item.cast[i];
        if (!p->image_tag[0]) continue;
        if (jf_download_item_image(&g_cfg, p->id, "Primary", p->image_tag, 48, POSTER_TMP))
            g_cast_px[i] = load_image_tmp(POSTER_TMP, &g_cast_px_w[i], &g_cast_px_h[i]);
    }
}

/* ── loading spinner (animated GIF, shown while (re)connecting to the
 * transcode stream — network stream isn't byte-range seekable, so seeking
 * means stop+restart with a new startTimeTicks, which has a few seconds of
 * reconnect latency worth covering with feedback) ───────────────────────── */

#define SPINNER_SIZE   14
#define SPINNER_MARGIN 10
#define SPINNER_BLINK_MS 350

/* Simple blinking square in the top-right corner — a GIF mascot animation
 * was tried first but wasn't worth the decode/ghosting complexity for what
 * is just a "something's loading" cue. */
static void draw_spinner_frame(FBDev *fb, int frame_idx)
{
    int dx = fb->width - SPINNER_SIZE - SPINNER_MARGIN;
    int dy = SPINNER_MARGIN;
    fb_fill_rect_alpha(fb, dx, dy, SPINNER_SIZE, SPINNER_SIZE, 0, 0, 0, 255);
    if (frame_idx % 2 == 0)
        fb_fill_rect_alpha(fb, dx, dy, SPINNER_SIZE, SPINNER_SIZE, 0x40, 0xE0, 0x40, 255);
}

typedef struct { int x, y; } SpinnerSamplePt;

/* Spread out, well clear of the spinner's own corner square. */
static const SpinnerSamplePt SPINNER_SAMPLE_PTS[] = {
    {60,60},{200,60},{340,60},{460,60},
    {60,140},{200,140},{340,140},{460,140},
    {60,220},{200,220},{340,220},{460,220},
};
#define SPINNER_SAMPLE_N (int)(sizeof(SPINNER_SAMPLE_PTS) / sizeof(SPINNER_SAMPLE_PTS[0]))

/* Shows the blinking indicator, polling for real video underneath instead
 * of blindly sleeping through the whole window — matters now that the wait
 * can be as long as 50s for the slow subtitle+seek case (see play()), but
 * most restarts are still the fast ~2s case and shouldn't be held up.
 * mplayer writes video frames directly to fb->mem, bypassing our fb->back
 * entirely — so fb->back can be stale (still holding whatever WE last
 * explicitly drew, e.g. the info screen, even minutes into playback) by the
 * time a seek calls this. Sync back from mem first so the overlay
 * composites onto what's ACTUALLY on screen right now (the frozen last
 * video frame), not onto stale leftover back-buffer content — confirmed on
 * hardware: without this sync, seeking made the screen jump back to the
 * info/cover page while "loading". */
static void spinner_show(FBDev *fb, double seconds)
{
    memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);

    uint32_t ref[SPINNER_SAMPLE_N];
    int valid[SPINNER_SAMPLE_N];
    for (int i = 0; i < SPINNER_SAMPLE_N; i++) {
        valid[i] = SPINNER_SAMPLE_PTS[i].x < fb->width && SPINNER_SAMPLE_PTS[i].y < fb->height;
        ref[i] = valid[i] ? *(const uint32_t *)(fb->mem + SPINNER_SAMPLE_PTS[i].y * fb->stride
                                                          + SPINNER_SAMPLE_PTS[i].x * 4) : 0;
    }

    double until = now_sec() + seconds;
    int frame_idx = 0;
    while (now_sec() < until) {
        if (frame_idx > 0) {
            int changed = 0;
            for (int i = 0; i < SPINNER_SAMPLE_N && !changed; i++) {
                if (!valid[i]) continue;
                uint32_t cur = *(const uint32_t *)(fb->mem + SPINNER_SAMPLE_PTS[i].y * fb->stride
                                                             + SPINNER_SAMPLE_PTS[i].x * 4);
                if (cur != ref[i]) changed = 1;
            }
            /* mplayer already drawing real frames underneath — stop
             * stomping fb->mem with our own stale overlay every cycle. */
            if (changed) return;
        }
        draw_spinner_frame(fb, frame_idx);
        fb_flip(fb);
        usleep(SPINNER_BLINK_MS * 1000);
        frame_idx++;
    }
}

/* ── browse frames ────────────────────────────────────────────────────────── */

/* The BaseItemKind that actually represents "one unit" of a library, keyed
 * off CollectionType — used both for the carousel's "N movies/series/
 * albums" count (jf_count_items) and its background cover grid
 * (jf_list_items_recursive): a plain direct-children listing of a by-artist
 * music library only finds MusicArtist folders, which often have no cover
 * art of their own, so both need to look past the top level at the real
 * leaf type. NULL (unrecognized collection type) means "don't filter". */
static const char *collection_item_type(const char *collection_type)
{
    if (!strcmp(collection_type, "movies"))  return "Movie";
    if (!strcmp(collection_type, "tvshows")) return "Series";
    if (!strcmp(collection_type, "music"))   return "MusicAlbum";
    return NULL;
}

static void fetch_frame(void)
{
    BrowseFrame *f = &g_stack[g_stack_depth - 1];
    switch (f->kind) {
    case FRAME_VIEWS:
        g_item_count = jf_list_views(&g_cfg, g_items, JF_MAX_ITEMS);
        for (int i = 0; i < g_item_count; i++) {
            const char *item_type = collection_item_type(g_items[i].collection_type);
            g_view_counts[i] = jf_count_items(&g_cfg, g_items[i].id, item_type);
        }
        break;
    case FRAME_ITEMS:
        g_item_count = jf_list_items(&g_cfg, f->parent_id, g_items, JF_MAX_ITEMS);
        for (int i = 0; i < g_item_count; i++)
            g_series_episode_counts[i] = (g_items[i].type == JF_TYPE_SERIES)
                ? jf_count_items(&g_cfg, g_items[i].id, "Episode") : -1;
        break;
    case FRAME_SEASONS:
        g_item_count = jf_list_seasons(&g_cfg, f->series_id, g_items, JF_MAX_ITEMS);
        break;
    case FRAME_EPISODES:
        g_item_count = jf_list_episodes(&g_cfg, f->series_id, f->season_id, g_items, JF_MAX_ITEMS);
        break;
    }
    g_sel = 0; g_scroll = 0;
    /* Returning to the root screen (B all the way back out of a library)
     * should land on whichever library you drilled into, not always snap
     * back to the first one — g_root_sel is kept up to date by the
     * carousel/list navigation itself (see the STATE_BROWSE input
     * handling), so just restore it here instead of the fresh-start 0
     * above. Left at 0 the first time the app ever reaches the root
     * (g_root_sel's own initializer). */
    if (f->kind == FRAME_VIEWS && g_item_count > 0) {
        g_sel = g_root_sel;
        if (g_sel >= g_item_count) g_sel = g_item_count - 1;
        if (g_sel < 0) g_sel = 0;
    }
}

static void push_frame(FrameKind kind, const char *title,
                        const char *parent_id, const char *series_id, const char *season_id)
{
    if (g_stack_depth >= MAX_STACK) return;
    BrowseFrame *f = &g_stack[g_stack_depth++];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    strncpy(f->title, title, sizeof(f->title) - 1);
    if (parent_id) strncpy(f->parent_id, parent_id, sizeof(f->parent_id) - 1);
    if (series_id) strncpy(f->series_id, series_id, sizeof(f->series_id) - 1);
    if (season_id) strncpy(f->season_id, season_id, sizeof(f->season_id) - 1);
    fetch_frame();
}

/* Returns 1 if popped back into a browse frame, 0 if already at the root
 * (caller should treat 0 as "exit app"). */
static int pop_frame(void)
{
    if (g_stack_depth <= 1) return 0;
    g_stack_depth--;
    fetch_frame();
    return 1;
}

/* ── drawing ──────────────────────────────────────────────────────────────── */

static const char *type_folder_icon(JfItemType t)
{
    switch (t) {
    case JF_TYPE_FOLDER: case JF_TYPE_SERIES: case JF_TYPE_SEASON:
    case JF_TYPE_ARTIST: case JF_TYPE_ALBUM: return "> ";
    default: return "";
    }
}

/* forward decl — defined below draw_info, reused here for the browse-list
 * cover panel (fit-preserving: a plain stretch-to-box distorts most
 * posters' 2:3-ish aspect ratio, confirmed visually on hardware). */
static void blit_fit_centered(FBDev *fb, const uint8_t *src, int sw, int sh,
                               int cx, int cy, int max_w, int max_h, uint8_t alpha);

/* Simple "flying through stars" background for the About screen — each
 * star has a depth (z) that shrinks every frame; projecting x/y by 1/z
 * makes it appear to accelerate toward the viewer, and it respawns at max
 * depth (from a random direction) once it passes the camera or drifts
 * off-screen. Pure decoration, redrawn fresh every frame since draw_about()
 * itself is already called on every loop tick while the screen is open. */
#define STAR_COUNT 40
typedef struct { float x, y, z; } Star;
static Star g_stars[STAR_COUNT];
static int  g_stars_init = 0;

static float star_frand_pm1(void) { return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f; }

static void star_respawn(Star *s)
{
    s->x = star_frand_pm1();
    s->y = star_frand_pm1();
    s->z = 1.0f;
}

static void draw_starfield(FBDev *fb)
{
    if (!g_stars_init) {
        g_stars_init = 1;
        for (int i = 0; i < STAR_COUNT; i++) {
            star_respawn(&g_stars[i]);
            g_stars[i].z = 0.05f + ((float)rand() / (float)RAND_MAX) * 0.95f;
        }
    }
    for (int i = 0; i < STAR_COUNT; i++) {
        Star *s = &g_stars[i];
        s->z -= 0.012f;
        if (s->z <= 0.05f) star_respawn(s);

        float scale = 1.0f / s->z;
        int sx = (int)(fb->width  / 2 + s->x * scale * (fb->width  / 4));
        int sy = (int)(fb->height / 2 + s->y * scale * (fb->height / 4));
        if (sx < 0 || sx >= fb->width || sy < 0 || sy >= fb->height) { star_respawn(s); continue; }

        int size = scale > 2.2f ? 2 : 1;   /* stars get a touch bigger as they approach */
        uint8_t bright = scale > 1.4f ? 255 : 150;
        fb_fill_rect_alpha(fb, sx, sy, size, size, bright, bright, bright, 255);
    }
}

/* Now-playing background effect #2 — simple falling rain/matrix-style
 * drops, same cost class as the starfield above (one small struct array,
 * a handful of fb_fill_rect_alpha calls per particle, no per-pixel loops).
 * Each drop is a short 3-pixel streak (brightest at the leading/bottom
 * edge, fading upward) rather than a single pixel, so it actually reads as
 * "falling" instead of just scattered static dots. */
#define RAIN_COUNT 60
typedef struct { float x, y, speed; uint8_t bright; } RainDrop;
static RainDrop g_rain[RAIN_COUNT];
static int      g_rain_init = 0;

static void rain_respawn(RainDrop *d, FBDev *fb, int initial)
{
    d->x     = (float)(rand() % fb->width);
    d->y     = initial ? (float)(rand() % fb->height) : -4.0f;
    d->speed = 1.5f + ((float)rand() / (float)RAND_MAX) * 2.5f;
    d->bright = (uint8_t)(120 + rand() % 136);
}

static void draw_rain(FBDev *fb)
{
    if (!g_rain_init) {
        g_rain_init = 1;
        for (int i = 0; i < RAIN_COUNT; i++) rain_respawn(&g_rain[i], fb, 1);
    }
    for (int i = 0; i < RAIN_COUNT; i++) {
        RainDrop *d = &g_rain[i];
        d->y += d->speed;
        if (d->y >= fb->height) rain_respawn(d, fb, 0);

        int sx = (int)d->x, sy = (int)d->y;
        uint8_t b = d->bright;
        /* Soft blue-white tint (vs. the starfield's plain white) so the two
         * effects read as distinct at a glance. */
        fb_fill_rect_alpha(fb, sx, sy,     1, 1, (uint8_t)(b*0.7f), (uint8_t)(b*0.8f), b, 255);
        fb_fill_rect_alpha(fb, sx, sy - 2, 1, 1, (uint8_t)(b*0.7f), (uint8_t)(b*0.8f), b, 140);
        fb_fill_rect_alpha(fb, sx, sy - 4, 1, 1, (uint8_t)(b*0.7f), (uint8_t)(b*0.8f), b, 70);
    }
}

/* Now-playing background effect #3 — MiSTer-Toasty-Squadron's own flying
 * toasters + moon, ported to match that project's actual screensaver
 * behavior (fixed diagonal down-left flight, varied sizes, a floating
 * sine-wave bob, a slow-drifting moon) rather than the generic random-
 * direction drift this had at first — confirmed against that project's own
 * sprite.c/anim.c/render.c/config.h for the exact movement math. Not a
 * full port of its Scene/SpriteInst pool (no spawn-rate ramping, no
 * mega-sprite overlap avoidance — this only ever runs a handful of
 * sprites, so neither matters here), but the same flight path/size tiers.
 * Lazily loads its frames on first use (same pattern as
 * grid_covers_sync/browse_cover_sync), covered by the corner spinner while
 * decoding since ~75 small PNGs + the moon takes a moment on this
 * hardware. */
#define TOASTY_SPEC_COUNT 15
#define TOASTY_FRAMES_MAX 43
static const struct { const char *dir; int frames; } TOASTY_SPECS[TOASTY_SPEC_COUNT] = {
    { "asset1",  21 },
    { "asset2",  32 },
    { "asset3",  11 },
    { "asset4",  32 },
    { "asset5",   1 },
    { "asset6",  43 },
    { "asset7",  32 },
    { "asset8",  23 },
    { "asset9",  39 },
    { "asset10", 33 },
    { "asset11", 39 },
    { "asset12", 39 },
    { "asset13", 42 },
    { "asset14", 18 },
    { "asset15", 42 },
};

static uint8_t *g_toasty_px[TOASTY_SPEC_COUNT][TOASTY_FRAMES_MAX];
static int      g_toasty_w[TOASTY_SPEC_COUNT][TOASTY_FRAMES_MAX];
static int      g_toasty_h[TOASTY_SPEC_COUNT][TOASTY_FRAMES_MAX];
static uint8_t *g_toasty_moon_px = NULL;
static int      g_toasty_moon_w = 0, g_toasty_moon_h = 0;
static int      g_toasty_loaded = 0;

static void toasty_load(FBDev *fb)
{
    if (g_toasty_loaded) return;
    g_toasty_loaded = 1;
    int spinner_frame = 0;
    for (int s = 0; s < TOASTY_SPEC_COUNT; s++) {
        for (int i = 0; i < TOASTY_SPECS[s].frames; i++) {
            char path[160];
            snprintf(path, sizeof(path), "/media/fat/misterfin/toasty/%s/%s_%d.png",
                      TOASTY_SPECS[s].dir, TOASTY_SPECS[s].dir, i + 1);
            int ch;
            g_toasty_px[s][i] = stbi_load(path, &g_toasty_w[s][i], &g_toasty_h[s][i], &ch, 4);
            if ((i & 3) == 0) { draw_spinner_frame(fb, spinner_frame++); fb_flip(fb); }
        }
    }
    int ch;
    g_toasty_moon_px = stbi_load("/media/fat/misterfin/toasty/moon.png",
                                  &g_toasty_moon_w, &g_toasty_moon_h, &ch, 4);
}

/* Toasty's own LAYER_CFG (config.h) — size in px, base flight speed and
 * per-spawn variance in px/sec, and render alpha. Picked with the same
 * weighting as that project's pick_layer(): small/far ones common, the
 * biggest "mega" tier rare. */
#define TOASTY_LAYER_COUNT 5
static const struct { int size; float base_speed, speed_var, alpha; } TOASTY_LAYERS[TOASTY_LAYER_COUNT] = {
    {  12, 10.0f,  2.0f, 0.20f },
    {  22, 18.0f,  4.0f, 0.60f },
    {  44, 28.0f,  6.0f, 1.00f },
    {  72, 44.0f,  8.0f, 1.00f },
    { 160, 72.0f, 14.0f, 1.00f },
};

static int toasty_pick_layer(void)
{
    float r = ((float)rand() / (float)RAND_MAX) * 100.0f;
    if (r < 23.0f) return 0;
    if (r < 45.0f) return 1;
    if (r < 63.0f) return 2;
    if (r < 88.0f) return 3;
    return 4;
}

/* MAX_SPRITES/SPAWN_RAMP_TIME/SPAWN_MAX/SPAWN_MIN — Toasty's own config.h
 * constants, verbatim. The pool starts empty and fills in at a rate that
 * ramps from one spawn every SPAWN_MAX seconds up to one every SPAWN_MIN
 * seconds over the first SPAWN_RAMP_TIME seconds (ease_inout_cubic,
 * matching anim.c's update_spawn()), reaching all 70 well before that
 * window closes. Everything here runs on real elapsed time (now_sec()
 * deltas), NOT a fixed per-call tick — a first version of this guessed at
 * a fixed tick-to-seconds scale and got the speed wrong; Toasty's own
 * speeds are already calibrated in real px/sec, so real dt is the only
 * way to actually match it exactly. */
#define TOASTY_MAX_SPRITES 70
#define TOASTY_SPAWN_RAMP_TIME 15.0
#define TOASTY_SPAWN_MAX 0.25
#define TOASTY_SPAWN_MIN 0.03
#define TOASTY_FRAME_DURATION (1.0 / 12.0)   /* config.h's FRAME_DURATION */

#define TOASTY_FLOAT_AMP_MIN 2.0f
#define TOASTY_FLOAT_AMP_MAX 5.0f

typedef struct {
    int   spec, layer;
    float x, y, vx, vy;
    int   frame;
    float frame_timer;
    float float_phase, float_speed, float_amp;
} ToastySprite;
static ToastySprite g_toasty_sprites[TOASTY_MAX_SPRITES];
static int          g_toasty_pool_size = 0;
static double       g_toasty_anim_start = 0.0;
static double       g_toasty_last_spawn = 0.0;
static double       g_toasty_last_tick  = 0.0;
static int          g_toasty_started = 0;

/* deck_shuffle()/deck_pick(), verbatim — one shuffled deck of all
 * TOASTY_SPEC_COUNT specs per layer, drawn from in order and reshuffled
 * once exhausted, so every sprite type shows up before any repeats
 * instead of a plain rand()%N occasionally clumping the same few. */
static int g_toasty_deck[TOASTY_LAYER_COUNT][TOASTY_SPEC_COUNT];
static int g_toasty_deck_idx[TOASTY_LAYER_COUNT];

static void toasty_deck_shuffle(int layer)
{
    for (int i = 0; i < TOASTY_SPEC_COUNT; i++) g_toasty_deck[layer][i] = i;
    for (int i = TOASTY_SPEC_COUNT - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = g_toasty_deck[layer][i];
        g_toasty_deck[layer][i] = g_toasty_deck[layer][j];
        g_toasty_deck[layer][j] = tmp;
    }
    g_toasty_deck_idx[layer] = 0;
}

static int toasty_deck_pick(int layer)
{
    if (g_toasty_deck_idx[layer] >= TOASTY_SPEC_COUNT) toasty_deck_shuffle(layer);
    return g_toasty_deck[layer][g_toasty_deck_idx[layer]++];
}

/* At most one mega-tier sprite active at a time — with the full 70-sprite
 * pool and a plain 12% per-spawn chance, 2-3 could otherwise end up
 * flying at once and fill the screen (confirmed by the user on hardware).
 * Toasty's own scene tolerates that at full density; this effect is meant
 * to stay a background decoration behind the cover/title/timeline, so a
 * spawn that rolls mega while one's already out gets bumped down to a
 * random non-mega tier instead of respecting that roll. */
static int toasty_mega_active(const ToastySprite *exclude)
{
    int mega_layer = TOASTY_LAYER_COUNT - 1;
    for (int i = 0; i < g_toasty_pool_size; i++) {
        if (&g_toasty_sprites[i] == exclude) continue;
        if (g_toasty_sprites[i].layer == mega_layer) return 1;
    }
    return 0;
}

static void toasty_sprite_spawn(ToastySprite *t, FBDev *fb)
{
    t->layer = toasty_pick_layer();
    if (t->layer == TOASTY_LAYER_COUNT - 1 && toasty_mega_active(t))
        t->layer = rand() % (TOASTY_LAYER_COUNT - 1);
    t->spec = toasty_deck_pick(t->layer);
    int size = TOASTY_LAYERS[t->layer].size;

    /* Always enters from the top edge or the right edge, off-screen —
     * matches sprite_spawn()'s fromTop/fromRight split (simplified to a
     * flat 60/40 rather than porting its full per-layer probability table
     * and mega-overlap-avoidance retry loop, which exist there to keep a
     * ~70-sprite scene from stacking same-layer mega sprites — a cosmetic
     * refinement, not the actual flight path/speed this was about). */
    float margin = (float)size;
    if (((float)rand() / (float)RAND_MAX) < 0.6f) {
        t->y = -margin - (float)(rand() % 60);
        t->x = -margin + ((float)rand() / (float)RAND_MAX) * (fb->width + 2.0f * margin);
    } else {
        t->x = (float)fb->width + margin + (float)(rand() % 60);
        t->y = -margin + ((float)rand() / (float)RAND_MAX) * (fb->height + 2.0f * margin);
    }

    /* Fixed ~45° down-left diagonal ± a few degrees of jitter, real px/sec
     * — confirmed against sprite_spawn()'s own angle = pi/4 ± 3°, vx
     * always negative, vy always positive, speed = LAYER_CFG's
     * baseSpeed ± speedVar with NO additional scaling. */
    float speed = TOASTY_LAYERS[t->layer].base_speed +
                   (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * TOASTY_LAYERS[t->layer].speed_var;
    float angle = (float)M_PI / 4.0f +
                  (((float)rand() / (float)RAND_MAX) * 7.0f - 3.0f) * (float)M_PI / 180.0f;
    t->vx = -fabsf(speed * cosf(angle));
    t->vy =  fabsf(speed * sinf(angle));

    t->frame = rand() % TOASTY_SPECS[t->spec].frames;
    t->frame_timer = 0.0f;
    t->float_phase = ((float)rand() / (float)RAND_MAX) * 360.0f;
    t->float_speed = 20.0f + ((float)rand() / (float)RAND_MAX) * 40.0f;   /* deg/sec */
    t->float_amp   = TOASTY_FLOAT_AMP_MIN +
                      ((float)rand() / (float)RAND_MAX) * (TOASTY_FLOAT_AMP_MAX - TOASTY_FLOAT_AMP_MIN);
}

static float toasty_ease_inout_cubic(float t)
{
    if (t < 0.5f) return 4.0f * t * t * t;
    float f = 2.0f * t - 2.0f;
    return f * f * f * 0.5f + 1.0f;
}

/* update_spawn(), verbatim ramp math. */
static void toasty_update_spawn(FBDev *fb, double now)
{
    if (g_toasty_pool_size >= TOASTY_MAX_SPRITES) return;
    float progress = (float)((now - g_toasty_anim_start) / TOASTY_SPAWN_RAMP_TIME);
    if (progress > 1.0f) progress = 1.0f;
    float eased = toasty_ease_inout_cubic(progress);
    double spawn_interval = TOASTY_SPAWN_MAX - eased * (TOASTY_SPAWN_MAX - TOASTY_SPAWN_MIN);
    if ((now - g_toasty_last_spawn) >= spawn_interval) {
        toasty_sprite_spawn(&g_toasty_sprites[g_toasty_pool_size], fb);
        g_toasty_pool_size++;
        g_toasty_last_spawn = now;
    }
}

/* Single moon instance, drifting left-to-right (the one thing NOT flying
 * down-left with the toasters) across a fixed 20%-60%-height band over
 * MOON_DURATION real seconds one-way — verbatim update_moon(). */
#define TOASTY_MOON_W 120
#define TOASTY_MOON_H 72
#define TOASTY_MOON_DURATION 120.0
static float  g_toasty_moon_x = 0, g_toasty_moon_y = 0;
static double g_toasty_moon_start = 0.0;
static int    g_toasty_moon_pending_y = 1;
static int    g_toasty_moon_on_screen = 0;

static void toasty_update_moon(FBDev *fb, double now)
{
    if (!g_toasty_moon_px) return;
    double elapsed = now - g_toasty_moon_start;
    if (elapsed >= TOASTY_MOON_DURATION) {
        g_toasty_moon_start = now;
        g_toasty_moon_x = -(float)TOASTY_MOON_W;
        g_toasty_moon_pending_y = 1;
        g_toasty_moon_on_screen = 0;
        elapsed = 0.0;
    }
    float progress = (float)(elapsed / TOASTY_MOON_DURATION);
    float start_x = -(float)TOASTY_MOON_W, end_x = (float)fb->width;
    g_toasty_moon_x = start_x + (end_x - start_x) * progress;

    if (g_toasty_moon_pending_y) {
        float min_y = fb->height * 0.20f, max_y = fb->height * 0.60f;
        g_toasty_moon_y = min_y + ((float)rand() / (float)RAND_MAX) * (max_y - min_y);
        g_toasty_moon_pending_y = 0;
    }
    g_toasty_moon_on_screen = (g_toasty_moon_x > -(float)TOASTY_MOON_W) && (g_toasty_moon_x < (float)fb->width);
}

static void toasty_sprite_draw(FBDev *fb, ToastySprite *t)
{
    uint8_t *px = g_toasty_px[t->spec][t->frame];
    if (!px) return;
    int size = TOASTY_LAYERS[t->layer].size;
    int dw = size;
    int dh = (int)(size * 0.6f);   /* PAL pixel-aspect correction, same factor
                                     * as Toasty's own PIXEL_ASPECT_R */
    int draw_y = (int)(t->y + sinf(t->float_phase * (float)M_PI / 180.0f) * t->float_amp);
    uint8_t layer_alpha = (uint8_t)(TOASTY_LAYERS[t->layer].alpha * 255.0f);
    fb_blit(fb, px, g_toasty_w[t->spec][t->frame], g_toasty_h[t->spec][t->frame],
            (int)t->x, draw_y, dw, dh, layer_alpha);
}

/* Background pass: advances EVERY active sprite (all layers, including the
 * mega tier — see draw_toasty_fg) and draws layers 0..TOASTY_LAYER_COUNT-2,
 * plus the moon. Called before the rest of the now-playing screen's own
 * UI. Smallest/farthest first so bigger "closer" sprites draw on top when
 * paths cross, same visual ordering as Toasty's own per-layer
 * render_bg() passes. */
static void draw_toasty_bg(FBDev *fb)
{
    /* Loading (447+1 PNGs) is triggered explicitly from the SELECT handler
     * that switches to this effect, not lazily here — doing it here meant
     * the multi-second decode's own spinner-flip loop was the first thing
     * to touch the framebuffer this tick, before the cover/title/timeline/
     * VU meters/hint below got a chance to draw, i.e. a blank screen with
     * just a spinner instead of the normal player UI staying up. Skipping
     * rendering while not yet loaded just leaves the plain black
     * fb_clear() background in place, which is exactly what was asked
     * for. */
    if (!g_toasty_loaded) return;
    double now = now_sec();
    if (!g_toasty_started) {
        g_toasty_started = 1;
        g_toasty_anim_start = now;
        g_toasty_last_spawn = now;
        g_toasty_last_tick  = now;
        g_toasty_moon_start = now;
        for (int l = 0; l < TOASTY_LAYER_COUNT; l++) toasty_deck_shuffle(l);
    }
    double dt = now - g_toasty_last_tick;
    g_toasty_last_tick = now;
    if (dt < 0.0 || dt > 0.25) dt = 0.0;   /* clamp a long pause/clock jump, not a real frame gap */

    toasty_update_spawn(fb, now);
    toasty_update_moon(fb, now);
    if (g_toasty_moon_on_screen)
        fb_blit(fb, g_toasty_moon_px, g_toasty_moon_w, g_toasty_moon_h,
                (int)g_toasty_moon_x, (int)g_toasty_moon_y, TOASTY_MOON_W, TOASTY_MOON_H, 255);

    for (int layer = 0; layer < TOASTY_LAYER_COUNT; layer++) {
        for (int i = 0; i < g_toasty_pool_size; i++) {
            ToastySprite *t = &g_toasty_sprites[i];
            if (t->layer != layer) continue;

            if (dt > 0.0) {
                t->x += t->vx * (float)dt;
                t->y += t->vy * (float)dt;
                t->frame_timer += (float)dt;
                if (t->frame_timer >= (float)TOASTY_FRAME_DURATION) {
                    t->frame_timer -= (float)TOASTY_FRAME_DURATION;
                    t->frame = (t->frame + 1) % TOASTY_SPECS[t->spec].frames;
                }
                t->float_phase += t->float_speed * (float)dt;
                if (t->float_phase >= 360.0f) t->float_phase -= 360.0f;
            }

            int size = TOASTY_LAYERS[t->layer].size;
            if (t->x < -(float)size || t->y > (float)fb->height + size)
                toasty_sprite_spawn(t, fb);

            /* Mega tier (last layer) flies OVER the cover/title/timeline/VU
             * meters, same as Toasty's own render_fg() drawing layer 4 over
             * its OSD — drawn separately by draw_toasty_fg(), after this
             * screen's own UI, using the state just advanced above. */
            if (layer == TOASTY_LAYER_COUNT - 1) continue;
            toasty_sprite_draw(fb, t);
        }
    }
}

/* Foreground pass: draws only the mega-tier sprites, on top of whatever
 * this screen has already drawn (cover art, title, timeline, VU meters,
 * hint text) — call this right before fb_flip(). Does NOT re-advance
 * position/frame/float state; draw_toasty_bg() already did that this
 * tick for every layer. */
static void draw_toasty_fg(FBDev *fb)
{
    int layer = TOASTY_LAYER_COUNT - 1;
    for (int i = 0; i < g_toasty_pool_size; i++) {
        ToastySprite *t = &g_toasty_sprites[i];
        if (t->layer == layer) toasty_sprite_draw(fb, t);
    }
}

/* Dims the now-playing screen from transparent at the top to solid black
 * at the very bottom — sits between the background effect and this
 * screen's own UI so a busy effect (Toasty's sprites, in particular)
 * doesn't fight with the timeline/VU meters/hint text for contrast. Only
 * used for that effect (see draw_now_playing) — starfield/rain are calm
 * enough already without it. */
static void draw_now_playing_gradient(FBDev *fb)
{
    for (int y = 0; y < fb->height; y++) {
        uint8_t a = (uint8_t)(255 * y / (fb->height - 1));
        fb_fill_rect_alpha(fb, 0, y, fb->width, 1, 0, 0, 0, a);
    }
}

/* show_footer=0 skips the "<version> installed"/update-available line and
 * the "B: back" hint — used by the --capture-about tool to render a clean
 * frame for a standalone marketing GIF (see tools/capture_about_gif.py). */
static void draw_about_frame(FBDev *fb, int show_footer)
{
    fb_clear(fb);
    draw_starfield(fb);

    static const char title[] = "MiSTerFin";
    static const char line1[] = "made over the weekends at pudding";
    static const char line2[] = "https://pudding.studio";

    static uint8_t *img_px = NULL;
    static int      img_w = 0, img_h = 0;
    static int      img_tried = 0;
    if (!img_tried) {
        img_tried = 1;
        int ch;
        img_px = stbi_load("/media/fat/misterfin/about.png", &img_w, &img_h, &ch, 4);
    }

    const int ts = 2, s1 = 1;
    const int tch = 8 * ts, sch = 8 * s1, img_gap = 10, lsp = 6;

    int img_h_box = 0, img_w_box = 0;
    if (img_px && img_w > 0 && img_h > 0) {
        int max_w = fb->width - 2 * SAFE_X;
        int max_h = 130;
        img_h_box = max_h < img_h ? max_h : img_h;
        img_w_box = (int)((double)img_w / img_h * img_h_box * 5.0 / 3.0 + 0.5);
        if (img_w_box > max_w) { img_h_box = img_h_box * max_w / img_w_box; img_w_box = max_w; }
    }

    int total_h = img_h_box + (img_h_box ? img_gap : 0) + tch + lsp + sch + lsp + sch;
    int avail_bottom = fb->height - 8 - SAFE_Y_BOT - 14 - 6;
    int cur_y = (avail_bottom - total_h) / 2 + 12;
    if (cur_y < SAFE_Y) cur_y = SAFE_Y;

    if (img_px && img_h_box > 0) {
        blit_fit_centered(fb, img_px, img_w, img_h,
                           fb->width / 2, cur_y + img_h_box / 2, img_w_box, img_h_box, 255);
        cur_y += img_h_box + img_gap;
    }

    draw_text(fb, (fb->width - text_width(title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp;
    draw_text(fb, (fb->width - text_width(line1, s1)) / 2, cur_y, line1, s1, COL_ITEM);
    cur_y += sch + lsp;
    draw_text(fb, (fb->width - text_width(line2, s1)) / 2, cur_y, line2, s1, COL_RESUME);

    if (show_footer) {
        pthread_mutex_lock(&g_upd_mutex);
        UpdateState us = g_upd_state;
        char latest[32];
        strncpy(latest, g_upd_latest, sizeof(latest) - 1);
        latest[sizeof(latest) - 1] = '\0';
        pthread_mutex_unlock(&g_upd_mutex);

        /* Same bottom margin as everything else now (SAFE_Y_BOT == SAFE_Y) —
         * not the taller margin MiSTerDVD's own about screen used. */
        int safe_y = fb->height - 8 - SAFE_Y_BOT;
        char installed[48];
        snprintf(installed, sizeof(installed), "%s installed", APP_VERSION);
        if (us == UPD_AVAILABLE) {
            draw_text(fb, SAFE_X, safe_y - 14, installed, 1, COL_DIM);
            char upd[64];
            snprintf(upd, sizeof(upd), "%s available", latest);
            draw_text(fb, SAFE_X, safe_y, upd, 1, COL_RESUME);
        } else {
            draw_text(fb, SAFE_X, safe_y, installed, 1, COL_DIM);
        }

        const char *hint = "B: back";
        draw_text(fb, fb->width - text_width(hint, 1) - SAFE_X, safe_y, hint, 1, COL_HINT);
    }

    fb_flip(fb);
}

static void draw_about(FBDev *fb) { draw_about_frame(fb, 1); }

/* Shown instead of the plain black config-error screen when jellyfin.conf
 * is missing/invalid or the configured username can't be resolved — same
 * starfield/cover-art chrome as the About screen (so it's not a jarring,
 * differently-styled dead end), with the About screen's own description
 * text swapped out for setup instructions. */
static void draw_setup_screen(FBDev *fb, const char *reason)
{
    fb_clear(fb);
    draw_starfield(fb);

    static const char title[] = "MiSTerFin";

    static uint8_t *img_px = NULL;
    static int      img_w = 0, img_h = 0;
    static int      img_tried = 0;
    if (!img_tried) {
        img_tried = 1;
        int ch;
        img_px = stbi_load("/media/fat/misterfin/about.png", &img_w, &img_h, &ch, 4);
    }

    const int ts = 2, s1 = 1;
    const int tch = 8 * ts, sch = 8 * s1, img_gap = 10, lsp = 6;

    int cur_y = SAFE_Y;
    if (img_px && img_w > 0 && img_h > 0) {
        int max_w = fb->width - 2 * SAFE_X;
        int max_h = 110;
        int img_h_box = max_h < img_h ? max_h : img_h;
        int img_w_box = (int)((double)img_w / img_h * img_h_box * 5.0 / 3.0 + 0.5);
        if (img_w_box > max_w) { img_h_box = img_h_box * max_w / img_w_box; img_w_box = max_w; }
        blit_fit_centered(fb, img_px, img_w, img_h,
                           fb->width / 2, cur_y + img_h_box / 2, img_w_box, img_h_box, 255);
        cur_y += img_h_box + img_gap;
    }

    draw_text(fb, (fb->width - text_width(title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp;
    draw_text(fb, (fb->width - text_width(reason, s1)) / 2, cur_y, reason, s1, COL_ERR);
    cur_y += sch + lsp * 2;

    const char *l1 = "Create /media/fat/misterfin/jellyfin.conf with:";
    draw_text(fb, (fb->width - text_width(l1, s1)) / 2, cur_y, l1, s1, COL_HINT);
    cur_y += sch + lsp;
    static const char *fields[] = { "server_url", "api_key", "username" };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        draw_text(fb, (fb->width - text_width(fields[i], s1)) / 2, cur_y, fields[i], s1, COL_ITEM);
        cur_y += sch + lsp;
    }

    const char *hint = "B: exit";
    draw_text(fb, (fb->width - text_width(hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* NOT a literal aspect-ratio box — blit_fit_centered's 5/3 pixel-aspect
 * correction means a typical portrait poster actually wants a box WIDER
 * than naive square-pixel math would suggest to look right on the real
 * screen (mirrors MiSTerDVD's own pdh=160/max_pw=200 proportions, scaled
 * down for this smaller corner panel). */
#define BROWSE_COVER_W 175
#define BROWSE_COVER_H 140

static uint8_t *g_browse_cover_px = NULL;
static int      g_browse_cover_w = 0, g_browse_cover_h = 0;
static char     g_browse_cover_item_id[JF_ID_LEN] = "";

/* Loads the currently-selected row's cover into the top-right panel, only
 * re-fetching when the selection actually changed (draw_browse redraws on
 * every keypress, not just navigation). */
static void browse_cover_sync(void)
{
    JfItem *it = (g_item_count > 0) ? &g_items[g_sel] : NULL;
    int wants_cover = it && it->image_tag[0] &&
        (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_EPISODE ||
         it->type == JF_TYPE_SERIES || it->type == JF_TYPE_SEASON ||
         it->type == JF_TYPE_ARTIST || it->type == JF_TYPE_ALBUM || it->type == JF_TYPE_TRACK);

    if (!wants_cover) {
        if (g_browse_cover_item_id[0]) {
            if (g_browse_cover_px) { stbi_image_free(g_browse_cover_px); g_browse_cover_px = NULL; }
            g_browse_cover_w = g_browse_cover_h = 0;
            g_browse_cover_item_id[0] = '\0';
        }
        return;
    }
    if (!strcmp(g_browse_cover_item_id, it->id)) return;   /* already loaded */

    if (g_browse_cover_px) { stbi_image_free(g_browse_cover_px); g_browse_cover_px = NULL; }
    g_browse_cover_w = g_browse_cover_h = 0;
    strncpy(g_browse_cover_item_id, it->id, sizeof(g_browse_cover_item_id) - 1);

    if (jf_download_item_image(&g_cfg, it->image_item_id, "Primary", it->image_tag, 180, POSTER_TMP))
        g_browse_cover_px = load_image_tmp(POSTER_TMP, &g_browse_cover_w, &g_browse_cover_h);
}

/* Clock (right-aligned, stopping short of the spinner's reserved corner —
 * SPINNER_SIZE+SPINNER_MARGIN — so a loading spinner never overlaps it) +
 * title (left-aligned, using whatever width is left before the clock).
 * Titles can get long (Album/Season ones are "Artist / Album" combos) —
 * scroll slowly instead of truncating when it doesn't fit, per user
 * request. Character-clipped via draw_text_clipped so it never draws into
 * the clock/spinner area. Shared by both the root carousel and the regular
 * list browse — both need the same header. */
/* Right-aligned, stopping short of the spinner's reserved corner
 * (SPINNER_SIZE+SPINNER_MARGIN) so a loading spinner never overlaps it.
 * Returns the clock's own left edge x, so callers that also draw a title
 * (draw_top_bar) know where they need to stop. */
static int draw_clock(FBDev *fb)
{
    time_t now_t = time(NULL);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);
    char clock_buf[8];
    snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", now_tm.tm_hour, now_tm.tm_min);
    int clock_right = fb->width - SPINNER_SIZE - SPINNER_MARGIN - 8;
    int clock_x = clock_right - text_width(clock_buf, 1);
    draw_text(fb, clock_x, SAFE_Y + 4, clock_buf, 1, COL_HINT);
    return clock_x;
}

static void draw_top_bar(FBDev *fb, const char *title)
{
    int clock_x = draw_clock(fb);

    int title_x0 = SAFE_X;
    int title_x1 = clock_x - 12;
    int title_w  = text_width(title, 2);
    if (strcmp(title, g_marquee_title) != 0) {
        strncpy(g_marquee_title, title, sizeof(g_marquee_title) - 1);
        g_marquee_title[sizeof(g_marquee_title) - 1] = '\0';
        g_marquee_px = 0.0;
    }
    if (title_w <= title_x1 - title_x0) {
        draw_text(fb, title_x0, SAFE_Y, title, 2, COL_TITLE);
    } else {
        int period = title_w + 40;   /* trailing gap before it loops back to the start */
        int off = (int)g_marquee_px % period;
        draw_text_clipped(fb, title_x0 - off, SAFE_Y, title, 2, COL_TITLE, title_x0, title_x1);
        draw_text_clipped(fb, title_x0 - off + period, SAFE_Y, title, 2, COL_TITLE, title_x0, title_x1);
    }
}

/* Drawn on top of whatever's already on screen (see the STATE_BROWSE input
 * handling) — doesn't clear or redraw the browse screen underneath itself,
 * same layering as the subtitle submenu's own overlay. */
static void draw_confirm_exit(FBDev *fb)
{
    const char *msg = "Exit? [A: yes  B: no]";
    int scale = 2;
    int tw = text_width(msg, scale);
    int th = 8 * scale;
    int cx = (fb->width - tw) / 2;
    int cy = (fb->height - th) / 2;
    fb_fill_rect_alpha(fb, cx - 12, cy - 12, tw + 24, th + 24, 0, 0, 0, 210);
    draw_text(fb, cx, cy, msg, scale, COL_TITLE);
    fb_flip(fb);
}

/* One card in the root library carousel — plain text, no icon/container per
 * user request (an earlier version had a placeholder icon tile + bordered
 * panel; both are gone now). All three cards (active + its two neighbors)
 * are the same size; only color marks which one is active. */
#define CAROUSEL_CARD_W 180   /* max text width before truncating */

static void draw_library_card(FBDev *fb, const JfItem *it, int64_t count, int cx, int cy, int active)
{
    char name[64];
    strncpy(name, it->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    truncate_to_width(name, 2, CAROUSEL_CARD_W);
    /* Can't ternary a multi-arg color macro straight into a call — the
     * comma in e.g. COL_TITLE splits into extra call arguments instead of
     * picking r/g/b together, silently mis-coloring this (caught by eye in
     * the --preview-browse render, not by the compiler). */
    if (active) draw_text(fb, cx - text_width(name, 2) / 2, cy - 10, name, 2, COL_TITLE);
    else        draw_text(fb, cx - text_width(name, 2) / 2, cy - 10, name, 2, 0xFF, 0xFF, 0xFF);

    if (count >= 0) {
        const char *ct = it->collection_type;
        char cbuf[32];
        if (!strcmp(ct, "movies"))
            snprintf(cbuf, sizeof(cbuf), "%lld movie%s", (long long)count, count == 1 ? "" : "s");
        else if (!strcmp(ct, "tvshows"))
            snprintf(cbuf, sizeof(cbuf), "%lld series", (long long)count);
        else if (!strcmp(ct, "music"))
            snprintf(cbuf, sizeof(cbuf), "%lld album%s", (long long)count, count == 1 ? "" : "s");
        else
            snprintf(cbuf, sizeof(cbuf), "%lld item%s", (long long)count, count == 1 ? "" : "s");
        draw_text(fb, cx - text_width(cbuf, 1) / 2, cy + 12, cbuf, 1, COL_HINT);
    }
}

/* Dimmed cover-art grid behind the carousel, tiling whatever the active
 * library actually contains — one library's worth of covers cached at a
 * time — but keeps EVERY library's grid it has ever loaded cached in its
 * own slot for the rest of the app session (not just the current one):
 * flipping back to a previously-visited library was re-downloading and
 * re-decoding its covers from scratch every single time otherwise (visible
 * as a lag spike per switch), confirmed as the cause by the user. Bounded
 * to GRID_LIB_CACHE_MAX slots — comfortably above any realistic number of
 * Jellyfin libraries, and each slot is only a handful of small (100px-wide)
 * decoded images, so the memory cost of never evicting is trivial on this
 * platform's RAM. Uses its own JfItem buffer (grid_items in
 * grid_covers_sync), NOT g_items — that array is the carousel's own
 * selection list and must not be clobbered by this side listing. */
#define GRID_FETCH_MAX 12
#define GRID_COLS 8
#define GRID_ROWS 4
#define GRID_ALPHA 65   /* out of 255 — dimmed but covers should read clearly, per user feedback that 40 was too faint */
#define GRID_LIB_CACHE_MAX 16

typedef struct {
    char     view_id[JF_ID_LEN];
    uint8_t *px[GRID_FETCH_MAX];
    int      w[GRID_FETCH_MAX], h[GRID_FETCH_MAX];
    int      count;
    /* Which cover (index into px[]) each grid cell shows — shuffled once
     * when this slot is first filled, NOT per draw: draw_browse_carousel
     * redraws every ~100ms just for the clock/marquee tick even with no
     * navigation, so reshuffling per-draw would make the background
     * visibly jitter. */
    int      cell_order[GRID_COLS * GRID_ROWS];
} GridLibCache;

static GridLibCache g_grid_cache[GRID_LIB_CACHE_MAX];
static int           g_grid_cache_n  = 0;    /* slots filled so far */
static int           g_grid_active   = -1;   /* index of the currently-shown library's slot, -1 = none */

/* A plain Fisher-Yates shuffle of the (count-cycled) index list still reads
 * as "repeating" with few unique covers spread over many cells (e.g. 4
 * covers over this grid's 32 cells is 8 copies of each) — nothing stops the
 * same cover landing in two vertically- or horizontally-adjacent cells,
 * which is exactly the visible pattern a uniform shuffle doesn't avoid.
 * Build the order cell-by-cell instead, rejecting a pick that matches the
 * cell directly above or to the left (a few retries, not exhaustive) —
 * cheap and enough to kill the obvious adjacent-repeat look; only gives up
 * (accepting a repeat) when there aren't enough distinct covers to satisfy
 * both neighbors at once. */
static void grid_cell_order_shuffle(GridLibCache *gc)
{
    if (gc->count <= 0) {
        for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) gc->cell_order[i] = 0;
        return;
    }
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int left  = col > 0 ? gc->cell_order[row * GRID_COLS + col - 1] : -1;
            int above = row > 0 ? gc->cell_order[(row - 1) * GRID_COLS + col] : -1;
            int pick, tries = 0;
            do {
                pick = rand() % gc->count;
            } while (gc->count > 2 && (pick == left || pick == above) && ++tries < 8);
            gc->cell_order[row * GRID_COLS + col] = pick;
        }
    }
}

/* Sequential downloads+decodes (up to GRID_FETCH_MAX) the first time a
 * given library is seen — covered by the corner spinner between steps,
 * same as info_assets_load's own sequential image fetches. Small request
 * width (100px) keeps each JPEG decode cheap since these only ever render
 * at ~80x72 tile size anyway. Already-cached libraries return immediately
 * (just a linear scan over however many are cached, at most
 * GRID_LIB_CACHE_MAX — trivial). */
static void grid_covers_sync(FBDev *fb, const JfItem *view)
{
    for (int i = 0; i < g_grid_cache_n; i++) {
        if (!strcmp(g_grid_cache[i].view_id, view->id)) { g_grid_active = i; return; }
    }
    if (g_grid_cache_n >= GRID_LIB_CACHE_MAX) { g_grid_active = -1; return; }

    GridLibCache *gc = &g_grid_cache[g_grid_cache_n];
    memset(gc, 0, sizeof(*gc));
    strncpy(gc->view_id, view->id, sizeof(gc->view_id) - 1);

    static JfItem grid_items[GRID_FETCH_MAX];
    const char *item_type = collection_item_type(view->collection_type);
    int n = jf_list_items_recursive(&g_cfg, view->id, item_type, grid_items, GRID_FETCH_MAX);

    int spinner_frame = 0;
    for (int i = 0; i < n && gc->count < GRID_FETCH_MAX; i++) {
        if (!grid_items[i].image_tag[0]) continue;
        draw_spinner_frame(fb, spinner_frame++); fb_flip(fb);
        if (jf_download_item_image(&g_cfg, grid_items[i].image_item_id, "Primary",
                                    grid_items[i].image_tag, 100, POSTER_TMP)) {
            uint8_t *px = load_image_tmp(POSTER_TMP, &gc->w[gc->count], &gc->h[gc->count]);
            if (px) gc->px[gc->count++] = px;
        }
    }
    grid_cell_order_shuffle(gc);
    g_grid_active = g_grid_cache_n++;
}

static void draw_grid_background(FBDev *fb)
{
    if (g_grid_active < 0) return;
    GridLibCache *gc = &g_grid_cache[g_grid_active];
    if (gc->count == 0) return;
    int cell_w = fb->width / GRID_COLS, cell_h = fb->height / GRID_ROWS;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int idx = gc->cell_order[row * GRID_COLS + col];
            fb_blit(fb, gc->px[idx], gc->w[idx], gc->h[idx],
                    col * cell_w, row * cell_h, cell_w, cell_h, GRID_ALPHA);
        }
    }
}

/* Vertical black gradient over the cover grid, under everything else (top
 * bar, cards, hint) — transparent at the top, solid at the bottom, per
 * user request, so the grid stays visible near the top but doesn't fight
 * with the hint text/cards lower down. */
static void draw_grid_gradient(FBDev *fb)
{
    for (int y = 0; y < fb->height; y++) {
        uint8_t a = (uint8_t)(255 * y / (fb->height - 1));
        fb_fill_rect_alpha(fb, 0, y, fb->width, 1, 0, 0, 0, a);
    }
}

/* Root screen (g_stack_depth == 1, FRAME_VIEWS) — a horizontal carousel of
 * library cards instead of the regular list, since a handful of libraries
 * (movies/TV/music, ...) reads better as a few big blocks than as rows.
 * Every library gets a slot along the strip (see draw_carousel_cards);
 * ones that don't fit on screen simply run off the edge and get clipped. */

/* Spacing between card centers along the strip. */
#define CAROUSEL_SPACING 150
#define CAROUSEL_SLIDE_STEPS 6     /* LEFT/RIGHT slide — see carousel_slide_animate() */

static int carousel_cy(FBDev *fb)
{
    int content_top = SAFE_Y + 24, content_bottom = fb->height - SAFE_Y_BOT - 28;
    return (content_top + content_bottom) / 2;
}

/* Draws every library card positioned relative to a (possibly fractional)
 * "visual selection" — an integer visual_sel is the normal at-rest layout;
 * a fractional one (from carousel_slide_animate) slides the whole strip
 * smoothly between two integer selections. The card nearest visual_sel
 * gets the active (yellow) treatment. */
static void draw_carousel_cards(FBDev *fb, double visual_sel, int cy)
{
    int center_cx = fb->width / 2;
    int active_i = (int)(visual_sel + (visual_sel >= 0 ? 0.5 : -0.5));
    for (int i = 0; i < g_item_count; i++) {
        double rel = i - visual_sel;
        int cx = center_cx + (int)(rel * CAROUSEL_SPACING);
        if (cx < -CAROUSEL_CARD_W || cx > fb->width + CAROUSEL_CARD_W) continue;
        if (i == active_i) continue;   /* drawn last, on top, in case of any overlap at tight spacing */
        draw_library_card(fb, &g_items[i], g_view_counts[i], cx, cy, 0);
    }
    if (active_i >= 0 && active_i < g_item_count) {
        int cx = center_cx + (int)((active_i - visual_sel) * CAROUSEL_SPACING);
        draw_library_card(fb, &g_items[active_i], g_view_counts[active_i], cx, cy, 1);
    }
}

static void draw_carousel_hint(FBDev *fb)
{
    const char *hint = "LEFT/RIGHT: browse   A:select   SELECT:list view   B:exit";
    draw_text(fb, (fb->width - text_width(hint,1))/2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);
}

/* Ease-out cubic — starts fast, decelerates into the resting position,
 * instead of the linear (constant-speed, mechanical-looking) motion the
 * first version of this had. */
static double carousel_ease(double t)
{
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

/* LEFT/RIGHT slide between old_sel and new_sel. Cards slide (eased) across
 * the whole animation; the background grid fades to black at the midpoint
 * and back in — which is also exactly when the grid is switched to the new
 * library, hidden behind full black so a slow first-time fetch for an
 * uncached library (grid_covers_sync's own download+decode pass) can't
 * show up as a jump-cut. A blocking loop, same as spinner_show()'s — no
 * manual delay beyond fb_flip()'s own vsync wait, which already paces this
 * to the display's real refresh rate (an explicit sleep on top of that
 * just made every step slower for no benefit, once fb_flip started
 * waiting for vsync itself). */
static void carousel_slide_animate(FBDev *fb, int old_sel, int new_sel)
{
    int cy = carousel_cy(fb);
    int switched = 0;
    for (int s = 1; s <= CAROUSEL_SLIDE_STEPS; s++) {
        double t = (double)s / CAROUSEL_SLIDE_STEPS;
        double visual = old_sel + (new_sel - old_sel) * carousel_ease(t);
        double fade_t = (t <= 0.5) ? (t / 0.5) : (1.0 - (t - 0.5) / 0.5);
        uint8_t black_alpha = (uint8_t)(255 * fade_t);

        if (!switched && t >= 0.5) {
            grid_covers_sync(fb, &g_items[new_sel]);
            switched = 1;
        }

        fb_clear(fb);
        draw_grid_background(fb);
        fb_fill_rect_alpha(fb, 0, 0, fb->width, fb->height, 0, 0, 0, black_alpha);
        draw_grid_gradient(fb);
        draw_top_bar(fb, "MiSTerFin");
        draw_carousel_cards(fb, visual, cy);
        draw_carousel_hint(fb);
        fb_flip(fb);
    }
}

static void draw_browse_carousel(FBDev *fb)
{
    if (g_item_count == 0) {
        fb_clear(fb);
        draw_top_bar(fb, "MiSTerFin");
        const char *msg = "No libraries found";
        draw_text(fb, (fb->width - text_width(msg, 1))/2, fb->height/2, msg, 1, COL_HINT);
        fb_flip(fb);
        return;
    }

    grid_covers_sync(fb, &g_items[g_sel]);

    fb_clear(fb);
    draw_grid_background(fb);
    draw_grid_gradient(fb);
    draw_top_bar(fb, "MiSTerFin");

    draw_carousel_cards(fb, (double)g_sel, carousel_cy(fb));
    draw_carousel_hint(fb);

    fb_flip(fb);
}

static void draw_browse(FBDev *fb)
{
    BrowseFrame *f = &g_stack[g_stack_depth - 1];
    if (f->kind == FRAME_VIEWS && !g_root_list_mode) { draw_browse_carousel(fb); return; }

    browse_cover_sync();

    fb_clear(fb);

    const char *title = f->title[0] ? f->title : "MiSTerFin";
    draw_top_bar(fb, title);

    if (g_item_count == 0) {
        const char *msg = "Nothing here";
        draw_text(fb, (fb->width - text_width(msg, 1))/2, fb->height/2, msg, 1, COL_HINT);
        fb_flip(fb);
        return;
    }

    int cover_panel_x = fb->width - SAFE_X - BROWSE_COVER_W;
    /* -3 lines the panel's top up with the first row's selection highlight
     * top (that highlight starts at y-3, see the is_sel rect below) —
     * fixed offset, the panel itself always stays put top-right regardless
     * of which row is actually selected. */
    int cover_panel_y = SAFE_Y + 24 - 3;
    int has_cover = (g_browse_cover_px != NULL);
    int row_max_w = (has_cover ? cover_panel_x - 10 : fb->width - SAFE_X) - SAFE_X;

    if (has_cover) {
        /* No placeholder background rect: a poster whose aspect doesn't
         * exactly match the box would otherwise show as visible grey
         * pillarbox bars. Letting the image be the only thing drawn means
         * any leftover margin just blends into whatever's already there. */
        int cx = cover_panel_x + BROWSE_COVER_W / 2;
        /* Top-align within the panel instead of vertically centering: a
         * portrait movie/series poster already fills BROWSE_COVER_H almost
         * exactly so this was never visible for those, but a square album
         * cover ends up noticeably SHORTER than the panel after the 5/3
         * fit-math below, and centering it left equal empty gaps top and
         * bottom instead of sitting flush with the panel's top edge like
         * the rest of the row layout expects. Replicate blit_fit_centered's
         * own dh calc here so cy can be derived from the actual resulting
         * height rather than the panel's fixed height. */
        int dh = BROWSE_COVER_H;
        int dw = (int)((double)g_browse_cover_w / g_browse_cover_h * dh * 5.0 / 3.0 + 0.5);
        if (dw > BROWSE_COVER_W) dh = dh * BROWSE_COVER_W / dw;
        int cy = cover_panel_y + dh / 2;
        blit_fit_centered(fb, g_browse_cover_px, g_browse_cover_w, g_browse_cover_h,
                           cx, cy, BROWSE_COVER_W, BROWSE_COVER_H, 255);
    }

    int end = g_scroll + VISIBLE;
    if (end > g_item_count) end = g_item_count;

    for (int i = g_scroll; i < end; i++) {
        int row = i - g_scroll;
        int y   = SAFE_Y + 24 + row * ROW_H;
        int is_sel = (i == g_sel);
        JfItem *it = &g_items[i];

        char line1[280];
        if (it->year[0] &&
            (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_SERIES))
            snprintf(line1, sizeof(line1), "%s%s (%s)", type_folder_icon(it->type), it->name, it->year);
        else
            snprintf(line1, sizeof(line1), "%s%s", type_folder_icon(it->type), it->name);
        truncate_to_width(line1, 1, row_max_w);

        /* Album: year + track count instead of a runtime — there's no
         * single "duration" for a whole album. Track: just its own
         * duration — no watched/resume state, which doesn't make sense for
         * an individual song the way it does for a movie/episode. Movie/
         * episode: unchanged runtime + watched/resume, per user request to
         * leave those as they were. */
        char line2[64] = {0};
        uint8_t l2r = 0x58, l2g = 0x58, l2b = 0x58;
        if (it->type == JF_TYPE_ALBUM) {
            if (it->year[0] && it->child_count > 0)
                snprintf(line2, sizeof(line2), "%s - %d track%s",
                         it->year, it->child_count, it->child_count == 1 ? "" : "s");
            else if (it->year[0])
                snprintf(line2, sizeof(line2), "%s", it->year);
            else if (it->child_count > 0)
                snprintf(line2, sizeof(line2), "%d track%s",
                         it->child_count, it->child_count == 1 ? "" : "s");
        } else if (it->type == JF_TYPE_TRACK) {
            fmt_time(line2, sizeof(line2), (double)it->runtime_ticks / 10000000.0);
        } else if (it->type == JF_TYPE_ARTIST) {
            if (it->child_count > 0)
                snprintf(line2, sizeof(line2), "%d album%s",
                         it->child_count, it->child_count == 1 ? "" : "s");
        } else if (it->type == JF_TYPE_SERIES) {
            /* Season count is it->child_count — free on the same request,
             * confirmed a Series' own ChildCount means exactly this (unlike
             * a top-level library view's, see jf_count_items's comment).
             * Episode count needs its own recursive query per series, done
             * once in fetch_frame() and cached in g_series_episode_counts. */
            int64_t ep = g_series_episode_counts[i];
            if (it->child_count > 0 && ep > 0)
                snprintf(line2, sizeof(line2), "%d season%s - %lld episode%s",
                         it->child_count, it->child_count == 1 ? "" : "s",
                         (long long)ep, ep == 1 ? "" : "s");
            else if (it->child_count > 0)
                snprintf(line2, sizeof(line2), "%d season%s",
                         it->child_count, it->child_count == 1 ? "" : "s");
        } else if (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_EPISODE) {
            int minutes = (int)(it->runtime_ticks / 10000000LL / 60);
            if (it->played) {
                snprintf(line2, sizeof(line2), "%d min - watched", minutes);
                l2r = 0x40; l2g = 0xCC; l2b = 0x40;
            } else if (it->resume_ticks > 0) {
                char tbuf[16];
                fmt_time(tbuf, sizeof(tbuf), (double)it->resume_ticks / 10000000.0);
                snprintf(line2, sizeof(line2), "%d min - resume %s", minutes, tbuf);
                l2r = 0xFF; l2g = 0xC0; l2b = 0x40;
            } else if (minutes > 0) {
                snprintf(line2, sizeof(line2), "%d min", minutes);
            }
        }
        truncate_to_width(line2, 1, row_max_w);

        if (is_sel) {
            fb_fill_rect_alpha(fb, SAFE_X - 4, y - 3,
                               row_max_w + 8, ROW_H - 2, COL_SEL_BG, 220);
            draw_text(fb, SAFE_X, y, line1, 1, COL_SEL_FG);
            if (line2[0]) draw_text(fb, SAFE_X, y + 11, line2, 1, l2r, l2g, l2b);
        } else {
            draw_text(fb, SAFE_X, y, line1, 1, COL_ITEM);
            if (line2[0]) draw_text(fb, SAFE_X, y + 11, line2, 1, l2r, l2g, l2b);
        }
    }

    if (g_item_count > VISIBLE) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", g_sel+1, g_item_count);
        draw_text(fb, fb->width - text_width(buf,1) - SAFE_X,
                  fb->height - 8 - SAFE_Y_BOT, buf, 1, COL_HINT);
    }

    int showing_artists = g_item_count > 0 && g_items[0].type == JF_TYPE_ARTIST;
    const char *hint =
        (g_stack_depth > 1 && showing_artists) ? "A:select  SELECT:shuffle library  B:back" :
        (g_stack_depth > 1)                    ? "A:select  B:back" :
                                                  "A:select  SELECT:cover view  B:exit";
    draw_text(fb, (fb->width - text_width(hint,1))/2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

#define HERO_H 150

/* Blits src into a max_w x max_h box, preserving its own real-world aspect
 * ratio and centering it (letterboxing/pillarboxing as needed — handles a
 * landscape screen-grab used as a poster fine, same as a normal portrait
 * one). Used for the logo and any poster/cover, which — unlike the
 * full-bleed backdrop crop-fill — must never look distorted.
 *
 * The 5/3 factor is MiSTerDVD's proven correction for this platform's
 * non-square pixels: our buffer is 640 wide feeding a 4:3 CRT through a
 * narrower final PAL/NTSC DDR resolution, so plain w/h aspect math alone
 * renders posters visibly too narrow ("elongated") on the real screen —
 * confirmed by reusing MiSTerDVD's exact fix (see its main.c cover-art
 * rendering) rather than re-deriving it. */
static void blit_fit_centered(FBDev *fb, const uint8_t *src, int sw, int sh,
                               int cx, int cy, int max_w, int max_h, uint8_t alpha)
{
    if (!src || sw <= 0 || sh <= 0) return;
    int dh = max_h;
    int dw = (int)((double)sw / sh * dh * 5.0 / 3.0 + 0.5);
    if (dw > max_w) { dh = dh * max_w / dw; dw = max_w; }
    fb_blit(fb, src, sw, sh, cx - dw / 2, cy - dh / 2, dw, dh, alpha);
}

static void draw_info(FBDev *fb)
{
    fb_clear(fb);

    /* Backdrop: full-bleed cover-crop (crop the taller dimension so the
     * remaining region already matches the target box's aspect ratio,
     * then a single uniform fb_blit stretch is distortion-free) — a plain
     * stretch-to-fill would visibly warp most 16:9-ish backdrops into our
     * much wider 640x150 box. */
    if (g_backdrop_px) {
        int crop_h = g_backdrop_w * HERO_H / fb->width;
        if (crop_h > g_backdrop_h) crop_h = g_backdrop_h;
        int skip = (g_backdrop_h - crop_h) / 2;
        const uint8_t *cropped = g_backdrop_px + (size_t)skip * g_backdrop_w * 4;
        fb_blit(fb, cropped, g_backdrop_w, crop_h, 0, 0, fb->width, HERO_H, 255);
    } else {
        fb_fill_rect_alpha(fb, 0, 0, fb->width, HERO_H, 0x18, 0x18, 0x18, 255);
    }

    /* Legibility gradient behind the logo/title: transparent at the
     * backdrop's midpoint, fading to near-opaque black by the bottom edge
     * so it blends smoothly into the solid black area below the hero
     * instead of showing a hard-edged bar. */
    int grad_top = HERO_H / 2;
    for (int gy = grad_top; gy < HERO_H; gy++) {
        int a = (gy - grad_top) * 220 / (HERO_H - grad_top);
        fb_fill_rect_alpha(fb, 0, gy, fb->width, 1, 0, 0, 0, (uint8_t)a);
    }

    if (g_logo_px) {
        /* max_w bumped from 280: the 5/3 PAR-corrected width for a wide
         * logo often exceeds that, which clamped height down below the
         * intended 34px target (confirmed on hardware — logos looked
         * smaller after the aspect fix). 480 covers essentially any real
         * logo's aspect ratio at the full target height. */
        blit_fit_centered(fb, g_logo_px, g_logo_w, g_logo_h,
                           fb->width / 2, HERO_H - 24, 480, 34, 255);
    } else {
        char title_line[300];
        if (g_info_item.year[0])
            snprintf(title_line, sizeof(title_line), "%s (%s)", g_info_item.name, g_info_item.year);
        else
            snprintf(title_line, sizeof(title_line), "%s", g_info_item.name);
        draw_text(fb, (fb->width - text_width(title_line, 1)) / 2, HERO_H - 28,
                  title_line, 1, COL_SEL_FG);
    }

    int ty = HERO_H + 8;
    int tw = fb->width - 2 * SAFE_X;

    char status[64] = {0};
    if (g_info_item.runtime_ticks > 0)
        snprintf(status, sizeof(status), "%d min",
                 (int)(g_info_item.runtime_ticks / 10000000LL / 60));
    if (g_info_item.played) {
        strncat(status, status[0] ? "  -  Watched" : "Watched", sizeof(status) - strlen(status) - 1);
    } else if (g_info_item.resume_ticks > 0) {
        char tbuf[16], rbuf[48];
        fmt_time(tbuf, sizeof(tbuf), (double)g_info_item.resume_ticks / 10000000.0);
        snprintf(rbuf, sizeof(rbuf), "%s  -  Resume %s", status[0] ? status : "", tbuf);
        strncpy(status, rbuf, sizeof(status) - 1);
    }
    if (status[0]) {
        if (g_info_item.played)                          draw_text(fb, SAFE_X, ty, status, 1, COL_WATCHED);
        else if (g_info_item.resume_ticks > 0)            draw_text(fb, SAFE_X, ty, status, 1, COL_RESUME);
        else                                              draw_text(fb, SAFE_X, ty, status, 1, COL_DIM);
        ty += 12;
    }

    if (g_info_item.overview[0])
        ty += draw_wrapped(fb, SAFE_X, ty, g_info_item.overview, 1, tw, 3, COL_ITEM);
    ty += 4;

    /* Cast row — small headshots only, no name labels: there isn't enough
     * vertical room left at this resolution for both a photo and legible
     * text per person. */
    int cast_n = g_info_item.cast_count;
    if (cast_n > CAST_DISPLAY_MAX) cast_n = CAST_DISPLAY_MAX;
    if (cast_n > 0) {
        int thumb = 34;
        int slot = tw / cast_n;
        for (int i = 0; i < cast_n; i++) {
            int cx = SAFE_X + slot * i + slot / 2;
            int cy = ty + thumb / 2;
            fb_fill_rect_alpha(fb, cx - thumb/2, cy - thumb/2, thumb, thumb, 0x20, 0x20, 0x20, 255);
            if (g_cast_px[i])
                fb_blit(fb, g_cast_px[i], g_cast_px_w[i], g_cast_px_h[i],
                        cx - thumb/2, cy - thumb/2, thumb, thumb, 255);
        }
    }

    const char *hint = (g_info_item.resume_ticks > 0 && !g_info_item.played)
        ? "A:resume  SELECT:restart  B:back"
        : "A:play  B:back";
    draw_text(fb, (fb->width - text_width(hint,1))/2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* Simple dashed track + a small square marker at the current position,
 * with "current / total" underneath — only safe to draw while mplayer
 * itself isn't actively writing frames (paused), same reasoning as the
 * spinner overlay: during active playback this would race mplayer's own
 * continuous frame writes and flicker. */
static void draw_timeline(FBDev *fb, int y, double pos, double duration)
{
    int x0 = SAFE_X, x1 = fb->width - SAFE_X;
    int barw = x1 - x0;
    int mx = x0;
    if (duration > 0) {
        double frac = pos / duration;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        mx = x0 + (int)(frac * barw);
    }
    /* Elapsed side brighter than the remaining side (was one flat gray
     * bar) so progress reads at a glance, not just from the numeric
     * label below — same bar/function for both video and audio. */
    if (mx > x0) fb_fill_rect_alpha(fb, x0, y, mx - x0, 2, 0xB8, 0xB8, 0xB8, 255);
    if (x1 > mx) fb_fill_rect_alpha(fb, mx, y, x1 - mx, 2, 0x30, 0x30, 0x30, 255);
    if (duration > 0)
        fb_fill_rect_alpha(fb, mx - 3, y - 4, 6, 10, 0xFF, 0xE0, 0x40, 255);
    char cur[16], tot[16], label[40];
    fmt_time(cur, sizeof(cur), pos);
    fmt_time(tot, sizeof(tot), duration);
    snprintf(label, sizeof(label), "%s / %s", cur, tot);
    draw_text(fb, (fb->width - text_width(label, 1)) / 2, y + 10, label, 1, COL_ITEM);
}

static void draw_paused(FBDev *fb, const char *name, double pos)
{
    memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);

    int cy = fb->height / 2 - 16;
    fb_fill_rect_alpha(fb, 0, cy - 6, fb->width, 50, 0, 0, 0, 200);

    const char *ps = "|| PAUSED";
    draw_text(fb, (fb->width - text_width(ps, 2)) / 2, cy, ps, 2, 0xFF, 0xFF, 0x00);

    char nbuf[64];
    snprintf(nbuf, sizeof(nbuf), "%.60s", name);
    draw_text(fb, (fb->width - text_width(nbuf, 1)) / 2, cy + 20, nbuf, 1, COL_ITEM);

    draw_timeline(fb, fb->height - 8 - SAFE_Y_BOT - 20, pos,
                  (double)g_info_item.runtime_ticks / 10000000.0);

    const char *hint = (g_info_item.sub_count > 0)
        ? "A:resume  B:stop  L/R:vsync  SELECT:subs"
        : "A:resume  B:stop  L/R:vsync";
    draw_text(fb, (fb->width - text_width(hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* ── playback (mplayer slave-mode, adapted from MiSTerDVD's non-DVD path) ── */

static pid_t   g_player_pid      = -1;
static int     g_cmd_fd          = -1;
static double  g_play_offset     = 0.0;
static double  g_play_start_wall = 0.0;
static int     g_paused          = 0;
static double  g_pause_wall      = 0.0;
static char    g_play_session_id[64];
static double  g_last_progress_report = 0.0;
/* -1 = off, otherwise JfSubtitle.index of the loaded/active track. Text
 * tracks are rendered client-side by mplayer (sub_load/sub_select — see
 * subtitle_load_client()) and don't need a restart; image-based tracks
 * (see g_burned_in_sub_index) do. Reset to -1 only when starting a title
 * fresh from the info screen — preserved across a seek-triggered restart of
 * the same title (seeks re-download and re-load the same subtitle file
 * after the restart, see play()). */
static int     g_current_sub_index = -1;
/* -1 = none, otherwise the JfSubtitle.index currently baked into the
 * playing stream's URL via jf_stream_url's burn_in_sub_index (see
 * subtitle_apply()). Kept in sync with g_current_sub_index whenever the
 * active track is image-based; read by play() on every (re)start, including
 * seek-triggered ones, so the burn-in survives seeks the same way
 * g_current_sub_index already does for client-rendered tracks. */
static int     g_burned_in_sub_index = -1;
/* Seek is a full stop+restart (network stream isn't byte-range seekable —
 * see player_seek) which takes a couple of seconds; accumulate repeated
 * presses into one seek instead of firing a restart per press, same as
 * MiSTerDVD's local-seek debounce pattern. */
static double  g_seek_accum  = 0.0;
static double  g_seek_fire_at = 0.0;   /* 0 = no pending seek */

/* Audio (music) playback — index into g_items of the currently playing
 * track, so LEFT/RIGHT/auto-advance can just walk the already-fetched
 * album/list rather than tracking a separate queue structure. Only valid
 * while state == STATE_PLAYING_AUDIO; g_items itself doesn't change
 * underneath it since browsing is paused during playback. */
static int      g_audio_queue_pos = -1;
static uint8_t *g_nowplaying_cover_px = NULL;
static int      g_nowplaying_cover_w = 0, g_nowplaying_cover_h = 0;
static char     g_nowplaying_cover_item_id[JF_ID_LEN] = "";
static double   g_vu_level_l = 0.0, g_vu_level_r = 0.0;   /* attack/decay state, see draw_vu_horizontal */

/* Requested once here so both play() and the subtitle/info overlay (which
 * displays it alongside the source's own specs) reference the same values. */
/* Tried native PAL (720x576) as an experiment — confirmed on hardware it's
 * not sustainable: A-V desync grew continuously (0.5s -> 0.65s+ within 5-6
 * seconds of playback, still climbing) and CPU sat near saturation. This
 * weak Cortex-A9 genuinely can't keep up at that resolution; back to the
 * known-good profile. */
/* Bumped from 5000000 to 8000000 — confirmed on hardware bitrate barely
 * moves CPU usage at this resolution (2Mbps and 8Mbps both landed at
 * ~34-35% avg utime, A-V desync stayed at 0.000 either way); resolution is
 * what actually costs CPU (see the native-PAL note above), so there's no
 * real reason not to spend the extra bitrate on quality here. */
static const JfStreamProfile g_stream_profile = { .max_width = 480, .max_height = 270, .video_bitrate = 8000000 };

static void mp_cmd(const char *cmd)
{
    if (g_cmd_fd >= 0) write(g_cmd_fd, cmd, strlen(cmd));
}

static double play_position(void)
{
    if (g_paused) return g_play_offset + (g_pause_wall - g_play_start_wall);
    return g_play_offset + (now_sec() - g_play_start_wall);
}

static int player_running(void)
{
    if (g_player_pid < 0) return 0;
    int status;
    if (waitpid(g_player_pid, &status, WNOHANG) > 0) { g_player_pid = -1; return 0; }
    return 1;
}

static void player_stop(void)
{
    if (g_cmd_fd >= 0) { mp_cmd("quit\n"); close(g_cmd_fd); g_cmd_fd = -1; }
    if (g_player_pid > 0) {
        usleep(150000);
        /* mplayer's -cache implementation forks a separate cache-fill
         * process rather than using a thread; that child is not tracked by
         * g_player_pid and survives killing just the main pid (confirmed on
         * hardware: repeated play/stop cycles left orphaned mplayer-arm
         * processes running indefinitely). play() puts the whole subtree in
         * its own process group via setpgid(), so kill the group. */
        kill(-g_player_pid, SIGKILL);
        waitpid(g_player_pid, NULL, 0);
        g_player_pid = -1;
    }
    g_paused = 0;

    /* mplayer's own vo_fbdev patch opens /dev/tty itself and restores
     * KD_TEXT (+ the console cursor) on its own graceful shutdown path —
     * which our "quit\n" above triggers before the SIGKILL even lands.
     * Without reclaiming graphics mode here, the console cursor blinks
     * through our subsequent drawing, and during a seek-restart (stop then
     * immediately play() again) the screen can show raw console text
     * instead of our spinner/video for a moment (confirmed on hardware). */
    cursor_hide();
}

static void player_pause_toggle(void)
{
    if (g_player_pid < 0) return;
    mp_cmd("pause\n");
    if (g_paused) {
        g_play_start_wall += now_sec() - g_pause_wall;
        g_paused = 0;
        /* Restore subtitle rendering now that our own pause-screen/submenu
         * overlay is gone — see the sub_visibility 0 branch below for why
         * this was hidden in the first place. Harmless if no subtitle is
         * actually loaded (g_current_sub_index < 0). */
        mp_cmd("sub_visibility 1\n");
    } else {
        g_pause_wall = now_sec();
        g_paused = 1;
        /* Subtitle text is drawn by mplayer's own draw_osd()/vo_draw_text()
         * on its own internal refresh schedule, independent of pause state
         * and independent of our own overlay draws — the two raced on the
         * same framebuffer memory with no coordination whenever a
         * subtitle happened to be showing at a paused position, confirmed
         * on hardware as the pause screen / subtitle submenu visibly
         * glitching. Hiding subs while our own overlay owns the screen
         * removes that race entirely (paired with -osdlevel 0 in play()
         * for the other, OSD-status half of it).
         *
         * pausing_keep is mandatory here: mplayer slave mode silently
         * resumes playback on ANY command issued while paused unless it's
         * prefixed with pausing_keep — confirmed on hardware (utime jumped
         * from a static ~24 ticks to 51 over 2s after sending a bare
         * sub_visibility 0 while paused). Without this prefix we were
         * unpausing the movie the instant we tried to hide its subtitles,
         * which is why pause silently stopped holding and why the
         * "pause screen" was actually racing live video underneath it. */
        mp_cmd("pausing_keep sub_visibility 0\n");
    }
}

static void play(FBDev *fb, const char *item_id, double offset_secs);   /* forward decl — used below */

/* Jellyfin's transcode stream is a plain progressive HTTP GET with
 * "Accept-Ranges: none" (confirmed against a real server) — there is no
 * byte offset to seek to, so mplayer's in-stream "seek" slave command is a
 * no-op on it. The only way to seek is to stop and re-request the stream
 * with a new startTimeTicks, same as a fresh play(). */
static void player_seek(FBDev *fb, double delta)
{
    double new_offset = play_position() + delta;
    if (new_offset < 0) new_offset = 0;
    player_stop();
    play(fb, g_info_item.id, new_offset);
}

#define SUB_LOCAL_PATH "/tmp/misterfin_sub.srt"

/* Subtitles are rendered client-side by mplayer, not burned in server-side —
 * an earlier version used subtitleMethod=Encode + a stream restart per
 * change, but that meant every subtitle change was really a seek, and
 * confirmed on a real server that seeking with burned-in subtitles forces
 * Jellyfin to decode+discard from the true start of the file up to the seek
 * target (a seek to 40:00 dropped exactly 40:00 worth of frames — on both
 * mpeg2video and h264, so not a codec issue) — a multi-minute stall on a
 * deep seek, and nothing a request parameter could avoid since it's the
 * server's own seek strategy once a subtitle filter is in its ffmpeg graph.
 * mplayer already has real subtitle rendering built in (sub_load/
 * sub_select in slave mode) and Jellyfin serves the raw .srt directly
 * (confirmed: no transcode needed for a text subtitle codec) — so we just
 * download the chosen track and hand it to mplayer, no restart at all. */
/* Our bitmap OSD font (same one mplayer's classic subtitle renderer falls
 * back to without FreeType — see -subpos comment in play()) only covers the
 * ASCII range MiSTerDVD's menus ever needed. A downloaded .srt can contain
 * anything — checked a real subtitle file and it was otherwise completely
 * ordinary English text except for a single "♪" (U+266A) used to mark
 * background music with no dialogue — that one non-ASCII character has no
 * glyph in this font and renders as garbage. Blanking any byte >= 0x80
 * drops it (and would drop accented characters in a non-English subtitle
 * too, which is a real loss, but showing garbage is worse and this font
 * can't render them either way). */
static void sanitize_srt_ascii(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > 2 * 1024 * 1024) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)len, f);
    for (size_t i = 0; i < got; i++)
        if ((unsigned char)buf[i] >= 0x80) buf[i] = ' ';

    fseek(f, 0, SEEK_SET);
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
}

/* The .srt has timestamps absolute to the ORIGINAL movie (e.g. 47:32), but
 * mplayer's own clock starts at ~0 for whatever point we asked Jellyfin to
 * start the transcode at (startTimeTicks) — confirmed via the server's own
 * ffmpeg command lines: a seeked stream's timestamps get reset near 0 in
 * the output regardless of whether it's an explicit setpts filter (burn-in
 * case) or ffmpeg's own default start_time normalization on a seeked input
 * (plain case, which is what we always use now). The offset between the
 * two clocks is exactly g_play_offset, so that part of the shift is exact.
 * AUDIO_DELAY_SEC and SUBTITLE_SYNC_FUDGE_SEC (see their own comments) cover
 * the rest of the fixed residual. g_sub_delay_extra is further live,
 * user-tunable adjustment on top of all three (see the submenu's LEFT/RIGHT
 * handling), for whatever a specific subtitle file's own drift still needs. */
static double g_sub_delay_extra = 0.0;   /* seconds, LEFT/RIGHT-adjustable in the submenu */

/* Both of these run while the submenu has the player paused (LEFT/RIGHT
 * live-tune sub_delay, A-confirm applies sub_remove/sub_load/sub_select) as
 * well as from a fresh, unpaused play() restart — pausing_keep is a no-op
 * when already playing, so it's always safe to use here rather than
 * threading g_paused through every call site. See player_pause_toggle()'s
 * sub_visibility comment for why the prefix is mandatory while paused. */
static void sub_delay_send(void)
{
    if (g_current_sub_index < 0) return;   /* nothing loaded yet — subtitle_load_client() will send it */
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "pausing_keep sub_delay %.3f 1\n",
             AUDIO_DELAY_SEC + SUBTITLE_SYNC_FUDGE_SEC - g_play_offset + g_sub_delay_extra);
    mp_cmd(cmd);
}

/* Loads a text-based subtitle client-side (mplayer sub_load/sub_select) —
 * no server/stream involvement, so this never needs a restart. Assumes
 * new_index is either -1 or a track that isn't burned in (see
 * subtitle_apply(), the only caller). */
static void subtitle_load_client(int new_index)
{
    if (new_index < 0) {
        mp_cmd("pausing_keep sub_remove\n");
        g_current_sub_index = -1;
        return;
    }

    /* Download BEFORE touching anything mplayer-side — if this fails
     * (network hiccup), the previous subtitle (if any) stays loaded and
     * selected instead of silently ending up with none. */
    if (!jf_download_subtitle(&g_cfg, g_info_item.id, g_info_item.id, new_index, SUB_LOCAL_PATH))
        return;
    sanitize_srt_ascii(SUB_LOCAL_PATH);

    mp_cmd("pausing_keep sub_remove\n");
    char cmd[112];
    snprintf(cmd, sizeof(cmd), "pausing_keep sub_load \"%s\"\n", SUB_LOCAL_PATH);
    mp_cmd(cmd);
    mp_cmd("pausing_keep sub_select 0\n");   /* sub_remove above guarantees this is the only loaded track */
    g_current_sub_index = new_index;
    sub_delay_send();
}

/* g_info_item.subs[] is unordered w.r.t. JfSubtitle.index (server-assigned,
 * not necessarily contiguous from 0) — this is the only way to get from an
 * index back to its codec. Returns NULL if index is -1 (off) or stale
 * (title changed underneath us), both of which callers treat as "no codec
 * info" and default to text. */
static const JfSubtitle *find_sub(int index)
{
    for (int i = 0; i < g_info_item.sub_count; i++)
        if (g_info_item.subs[i].index == index) return &g_info_item.subs[i];
    return NULL;
}

/* Picks client-side rendering vs. server burn-in per new_index's codec (see
 * jf_subtitle_is_text()) and restarts the stream via play() whenever that
 * choice changes what's baked into the URL — same stop+reopen mechanism as
 * player_seek(), since there is no in-place way to add/drop a server-side
 * burn-in on a live, non-seekable progressive stream. Switching between two
 * text tracks, or turning a text track off, stays restart-free exactly like
 * before. */
static void subtitle_apply(FBDev *fb, int new_index)
{
    int new_burn_in = -1;
    if (new_index >= 0) {
        const JfSubtitle *s = find_sub(new_index);
        if (s && !jf_subtitle_is_text(s->codec)) new_burn_in = new_index;
    }

    if (new_burn_in != g_burned_in_sub_index) {
        double pos = play_position();
        g_burned_in_sub_index = new_burn_in;
        g_current_sub_index   = new_index;
        player_stop();
        play(fb, g_info_item.id, pos);
        return;
    }

    subtitle_load_client(new_index);
}

/* Cycling with an immediate apply per SELECT press caused a cascade when
 * the old burn-in restart was still in play (each press interrupted the
 * previous restart before it connected, showing as a freeze) — kept the
 * menu even after switching to client-side rendering since a menu is still
 * nicer than blind cycling, though apply is instant now either way. */
static int    g_submenu_visible    = 0;
static int    g_submenu_sel        = 0;   /* 0 = Off, i+1 = g_info_item.subs[i] */
static int    g_submenu_was_paused = 0;   /* pause state before the menu opened, to restore on close */

static void submenu_open(FBDev *fb)
{
    g_submenu_visible    = 1;
    g_submenu_sel         = g_current_sub_index < 0 ? 0 : g_current_sub_index + 1;
    g_submenu_was_paused  = g_paused;
    if (!g_paused) player_pause_toggle();
    memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);
}

static void submenu_close(void)
{
    g_submenu_visible = 0;
    if (!g_submenu_was_paused && g_paused) player_pause_toggle();
}

static void draw_submenu(FBDev *fb)
{
    int n_opts = g_info_item.sub_count + 1;   /* JF_MAX_SUBS+1 = 9 max */

    int box_w = 280;
    /* Must exactly match every increment below (header, n_opts rows, sync
     * line, gap, 3 hint lines, padding) — computed directly from those same
     * numbers instead of separately, so it can't drift out of sync with the
     * actual content again like it did before (text drawn past the box's
     * own bottom edge). */
    int box_h = 10 + 15 + n_opts * 15 + 15 + 8 + 3 * 12 + 8;
    int box_x = (fb->width - box_w) / 2, box_y = fb->height / 2 - box_h / 2;
    fb_fill_rect_alpha(fb, box_x, box_y, box_w, box_h, 0, 0, 0, 225);

    int list_x = box_x + 12, list_y = box_y + 10;
    int label_max_w = box_w - 24;   /* box_w minus left/right margin, for truncation below */
    draw_text(fb, list_x, list_y, "SUBTITLES", 1, COL_HINT);
    list_y += 15;

    for (int i = 0; i < n_opts; i++) {
        int is_off  = (i == 0);
        int active  = is_off ? (g_current_sub_index < 0) : (g_current_sub_index == i - 1);
        const char *label = is_off ? "Off" : g_info_item.subs[i - 1].label;
        if (!label || !label[0]) label = "Unknown";

        if (i == g_submenu_sel)
            fb_fill_rect_alpha(fb, box_x + 4, list_y - 2, box_w - 8, 14, COL_SEL_BG, 220);

        char line[56];
        snprintf(line, sizeof(line), "%s%s", active ? "> " : "  ", label);
        /* A long DisplayTitle from the server (seen in practice: things
         * like "Undefined - SUBRIP - External") drawn past the box's own
         * width made the text stick out past the dark background behind
         * it — clip it to fit instead. */
        truncate_to_width(line, 1, label_max_w);
        if (active) draw_text(fb, list_x, list_y, line, 1, COL_RESUME);
        else        draw_text(fb, list_x, list_y, line, 1, COL_ITEM);
        list_y += 15;
    }

    char syncline[32];
    snprintf(syncline, sizeof(syncline), "Sync: %+.1fs", g_sub_delay_extra);
    draw_text(fb, list_x, list_y, syncline, 1, COL_ITEM);
    list_y += 15 + 8;   /* + gap before the hint block */

    const char *hint1 = "UP/DOWN: select subtitle";
    const char *hint2 = "LEFT/RIGHT: adjust subtitle offset";
    const char *hint3 = "A: apply    B: cancel";
    draw_text(fb, box_x + (box_w - text_width(hint1, 1)) / 2, list_y, hint1, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(hint2, 1)) / 2, list_y, hint2, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(hint3, 1)) / 2, list_y, hint3, 1, COL_HINT);

    fb_flip(fb);
}

static void submenu_confirm(FBDev *fb)
{
    int new_index = g_submenu_sel == 0 ? -1 : g_info_item.subs[g_submenu_sel - 1].index;
    submenu_close();
    if (new_index == g_current_sub_index) return;   /* no actual change */
    subtitle_apply(fb, new_index);
}

/* play_position() plus whatever seek is currently accumulating but hasn't
 * fired yet, clamped to the title's actual runtime — used both to show a
 * live-updating target while accumulating (draw_paused/draw_timeline) and
 * to compute the ">> Ns" flash below. Equals plain play_position() when
 * nothing is accumulating (g_seek_accum == 0), so callers can use this
 * unconditionally instead of branching on whether a seek is in progress. */
static double seek_pending_target(void)
{
    double duration = (double)g_info_item.runtime_ticks / 10000000.0;
    double target = play_position() + g_seek_accum;
    if (target < 0) target = 0;
    if (duration > 0 && target > duration) target = duration;
    return target;
}

/* Brief on-screen message via mplayer's OWN OSD text command, not our own
 * drawing. We tried drawing this ourselves directly into the shared
 * framebuffer, but while actively PLAYING, mplayer's decoder is
 * independently writing fresh video into that same buffer on its own
 * schedule — every one of our draws was racing that and losing roughly half
 * the time, which showed as constant flicker no matter how often we
 * redrew (confirmed on hardware; not fixable from our side without patching
 * mplayer's video driver, which needs a cross-compiler/Docker toolchain not
 * available here). osd_show_text has mplayer itself composite the text into
 * its own frame before writing it out, so there's only one writer and no
 * race — confirmed working on hardware. Trade-off: no custom background
 * box, just plain text over the video, since that's all mplayer's own OSD
 * renderer draws.
 *
 * pausing_keep is required if this might fire while paused (VSync toggle
 * can) — see player_pause_toggle()'s sub_visibility comment for why. */
static void osd_flash(const char *text, int paused)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%sosd_show_text \"%s\" %d\n",
             paused ? "pausing_keep " : "", text, (int)(FLASH_DURATION_SEC * 1000));
    mp_cmd(cmd);
}

/* Accumulates a pending seek and (re)starts the debounce window — the
 * actual restart fires from the main loop once SEEK_DEBOUNCE elapses with
 * no further presses. */
static void seek_accumulate(double delta, double now)
{
    g_seek_accum  += delta;
    g_seek_fire_at = now + SEEK_DEBOUNCE;
    if (g_paused) return;   /* draw_paused()'s own redraw already reflects this, see seek_pending_target() */

    int secs = (int)(g_seek_accum < 0 ? -g_seek_accum : g_seek_accum);
    char cur[16], tot[16], osd[80];
    fmt_time(cur, sizeof(cur), seek_pending_target());
    fmt_time(tot, sizeof(tot), (double)g_info_item.runtime_ticks / 10000000.0);
    snprintf(osd, sizeof(osd), "%s %ds  -  %s / %s",
             g_seek_accum >= 0 ? ">>" : "<<", secs, cur, tot);
    osd_flash(osd, 0);
}

static void play(FBDev *fb, const char *item_id, double offset_secs)
{
    g_seek_accum = 0.0;
    g_seek_fire_at = 0.0;

    /* Deliberately does NOT clear /dev/fb0 here — whatever's already on
     * screen (info screen, or the last video frame before a seek-restart)
     * should stay visible behind the loading spinner below. The -vf chain's
     * dsize=640:288 (see execlp below) makes mplayer's own frames fill the
     * whole framebuffer exactly once they start, so nothing needs pre-clearing. */

    /* Cortex-A9 is weak and this mplayer/ffmpeg build has no NEON-accelerated
     * YUV->BGRA colorspace conversion (confirmed on real hardware: swscale
     * falls back to a scalar C path regardless of scaler algorithm), and
     * A/V fell behind in real time at higher decode resolutions — so we
     * request a small decode from the server (cheap for the CPU) and let
     * mplayer's own -vf chain (see execlp below) scale/letterbox it back up
     * to fill /dev/fb0 afterwards (cheap relative to decode). */
    /* CPU wasn't saturated at 2Mbps mpeg2video (measured live on hardware:
     * ~45% usr, 50%+ idle) — testing a bump to 5Mbps to see if there's
     * headroom for a quality bump too. */
    int64_t start_ticks = (int64_t)(offset_secs * 10000000.0);
    jf_make_play_session_id(g_play_session_id, sizeof(g_play_session_id));

    char url[600];
    jf_stream_url(&g_cfg, item_id, &g_stream_profile, start_ticks, g_play_session_id,
                  g_burned_in_sub_index, url, sizeof(url));

    char delay_arg[16];
    snprintf(delay_arg, sizeof(delay_arg), "%.2f", AUDIO_DELAY_SEC);

    g_play_offset     = offset_secs > 0.0 ? offset_secs : 0.0;
    g_play_start_wall = now_sec();
    g_paused          = 0;
    g_last_progress_report = now_sec();
    jf_report_start(&g_cfg, item_id, g_play_session_id, start_ticks);

    int pfd[2];
    pipe(pfd);

    g_player_pid = fork();
    if (g_player_pid == 0) {
        setpgid(0, 0);   /* own process group, so player_stop() can kill mplayer's cache-fill child too */
        dup2(pfd[0], 0);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        nice(-5);
        execlp(MPLAYER, "mplayer",
               "-slave", "-quiet",
               "-nojoystick", "-noconsolecontrols",
               "-vo", "fbdev:/dev/fb0",
               "-ao", "alsa",
               /* osdlevel 0: mplayer's native seekbar/status OSD (shown by
                * default on pause/seek slave commands) draws directly into
                * the framebuffer on its own internal refresh schedule,
                * completely independent of our own overlay draws (pause
                * screen, subtitle submenu) — the two raced on the same
                * memory with no coordination, confirmed on hardware as
                * visible flicker/glitching. This kills that trigger; our
                * own drawn UI (draw_paused, draw_timeline) replaces it
                * entirely, see seek_accumulate(). Subtitle rendering is a
                * separate mechanism (not gated by osdlevel) — see
                * player_pause_toggle()'s sub_visibility toggle for that
                * other half of the same race. */
               "-osdlevel", "0",
               "-font", "/media/fat/misterfin/font/font.desc",
               "-framedrop",
               "-autosync", "30",
               "-cache", "8192", "-cache-min", "20",
               /* mplayer's own native MPEG-TS demuxer sometimes misses the
                * video PID on Jellyfin's transcoded TS output (only finds
                * audio) — forcing the ffmpeg/libavformat demuxer instead
                * fixes it. Confirmed against a real Jellyfin 10.11 server. */
               "-demuxer", "lavf",
               /* -sws 0 = fast bilinear instead of the (much costlier)
                * default bicubic scaler — confirmed empirically to matter
                * a lot given there's no NEON path in this build. */
               "-sws", "0",
               /* Decode is requested small server-side (see profile below)
                * to keep the software H.264 decode cheap; this -vf chain
                * scales it back up to fill /dev/fb0's native 640x288
                * (cheap relative to decode) while preserving the source's
                * own aspect ratio and letterboxing/pillarboxing with black
                * bars — scale=640:-1 fits the wider dimension, expand pads
                * the other to 640x288, and dsize=640:288 is required last:
                * without it mplayer's own internal aspect "prescale" logic
                * recomputes and overrides the final display size (confirmed
                * on hardware — omitting dsize gave a distorted stretch or a
                * wrong-sized output depending on the rest of the chain). */
               "-vf", "scale=640:-1,expand=640:288:-1:-1:1,dsize=640:288",
               "-lavdopts", "threads=2:fast",
               "-af", "format=s16le",
               /* This build has no FreeType/fontconfig (confirmed on
                * hardware: -subfont-osd-scale/-subfont-text-scale are both
                * "Unknown option"), so without -subfont, subtitles would
                * render through the same classic bitmap-font path as our
                * own OSD text — same (too large for a subtitle line) size.
                * -subfont points at a SEPARATE, smaller font generated
                * specifically for subtitles (tools/gen_subfont.py — same
                * font8x8 glyphs as the OSD font, but supersampled+
                * downsampled to ~10px with antialiased edges instead of
                * the OSD font's hard 16px blocks) so the OSD itself stays
                * unaffected. -subwidth caps subtitle line width to 90% of
                * the screen so mplayer wraps instead of letting a long
                * line run off-screen (confirmed accepted by this build,
                * unlike the FreeType-only scale options above).
                * -subpos sets the baseline as a % of screen height (100 =
                * very bottom edge); lined up with our own hint-bar row
                * (fb->height - 8 - SAFE_Y_BOT, ~90% at the 288-tall PAL
                * output) per user request, after confirming 88 read as too
                * high up. */
               "-subfont", "/media/fat/misterfin/subfont/font.desc",
               "-subwidth", "90",
               "-subpos", "92",
               /* Audio consistently trails video by a small fixed amount
                * (reported by the user across titles — not load-dependent,
                * so not the same issue fb_wait_vsync() fixed). First guess
                * was -0.10 — confirmed on hardware to make the gap BIGGER,
                * so the sign was backwards for this mplayer build. Flipped
                * to positive and roughly doubled the magnitude (since the
                * wrong-signed -0.10 visibly widened the gap by about that
                * much) — needs live tuning against further hardware
                * feedback, sign/magnitude both still empirical. */
               "-delay", delay_arg,
               url,
               (char *)NULL);
        _exit(1);
    }

    close(pfd[0]);
    g_cmd_fd = pfd[1];

    /* mplayer is connecting + filling its cache in the background at this
     * point and hasn't touched /dev/fb0 yet — safe window to show the
     * loading spinner without racing its own frame writes. */
    spinner_show(fb, 2.0);

    /* A client-rendered (text) subtitle selection survives a seek-triggered
     * restart (see player_seek) but the fresh mplayer instance doesn't have
     * anything loaded yet — re-download+load it the same way
     * subtitle_load_client() does for a manual change. An image-based
     * (burned-in) selection needs no client-side action at all here: it's
     * already baked into the url built above via g_burned_in_sub_index. */
    if (g_current_sub_index >= 0 && g_burned_in_sub_index < 0)
        subtitle_load_client(g_current_sub_index);
}

/* ── music playback (audio-only, direct play — see jf_audio_stream_url) ──── */

/* g_items[queue_pos] must be a JF_TYPE_TRACK; reuses player_stop()/
 * player_pause_toggle()/play_position() unchanged (those only touch
 * g_player_pid/g_cmd_fd/g_play_offset/g_paused, none of which are
 * video-specific) — only the mplayer invocation and on-screen UI differ
 * from play(). No video output at all (-novideo), so there's no fbdev race
 * to worry about the way play()'s comments describe; the now-playing
 * screen is entirely our own drawing, redrawn every loop tick like the
 * paused/about screens. */
static void play_audio(FBDev *fb, int queue_pos)
{
    (void)fb;
    JfItem *it = &g_items[queue_pos];
    g_audio_queue_pos = queue_pos;
    g_vu_level_l = g_vu_level_r = 0.0;

    unlink(AF_EXPORT_PATH);   /* don't let the visualizer read a stale previous track's data */

    jf_make_play_session_id(g_play_session_id, sizeof(g_play_session_id));
    char url[600];
    jf_audio_stream_url(&g_cfg, it->id, g_play_session_id, url, sizeof(url));

    g_play_offset     = 0.0;
    g_play_start_wall = now_sec();
    g_paused          = 0;
    g_last_progress_report = now_sec();
    jf_report_start(&g_cfg, it->id, g_play_session_id, 0);

    int pfd[2];
    pipe(pfd);

    g_player_pid = fork();
    if (g_player_pid == 0) {
        setpgid(0, 0);
        dup2(pfd[0], 0);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        nice(-5);
        execlp(MPLAYER, "mplayer",
               "-slave", "-quiet",
               "-nojoystick", "-noconsolecontrols",
               "-novideo",
               "-ao", "alsa",
               /* export=...:512 gives the now-playing visualizer 512 s16le
                * samples per channel to read from a live-updated mmap'd
                * file — confirmed supported on this mplayer build (tested
                * directly with a real FLAC track on hardware). */
               "-af", "export=" AF_EXPORT_PATH ":512,format=s16le",
               url,
               (char *)NULL);
        _exit(1);
    }

    close(pfd[0]);
    g_cmd_fd = pfd[1];

    if (strcmp(g_nowplaying_cover_item_id, it->id) != 0) {
        if (g_nowplaying_cover_px) { stbi_image_free(g_nowplaying_cover_px); g_nowplaying_cover_px = NULL; }
        g_nowplaying_cover_w = g_nowplaying_cover_h = 0;
        strncpy(g_nowplaying_cover_item_id, it->id, sizeof(g_nowplaying_cover_item_id) - 1);
        if (it->image_tag[0] &&
            jf_download_item_image(&g_cfg, it->image_item_id, "Primary", it->image_tag, 300, POSTER_TMP))
            g_nowplaying_cover_px = load_image_tmp(POSTER_TMP, &g_nowplaying_cover_w, &g_nowplaying_cover_h);
    }
}

/* Reads mplayer's live -af export file fresh every call (it's tiny — a
 * couple KB — cheap enough to re-read at redraw rate). hdr[0]/hdr[1]
 * (nch/sz) don't match this build's actual export size (confirmed on
 * hardware the file is only ~2KB total, while nch=2,sz=2048 would imply
 * 8KB of samples alone) — rather than guess the real header semantics,
 * just read however many int16 samples are ACTUALLY present after the
 * header and treat them as one flat buffer. Returns sample count, 0 if
 * the file isn't there yet (e.g. mplayer hasn't started exporting within
 * the first frame or two of a track). */
static int read_af_samples(int16_t *buf, int max_samples)
{
    FILE *f = fopen(AF_EXPORT_PATH, "rb");
    if (!f) return 0;
    int32_t hdr[2];
    if (fread(hdr, sizeof(int32_t), 2, f) != 2) { fclose(f); return 0; }
    size_t got = fread(buf, sizeof(int16_t), (size_t)max_samples, f);
    fclose(f);
    return (int)got;
}

/* Classic recording-console-style horizontal level meter: dim background
 * track, filled from the left up to the current peak level, colored by
 * ZONE along the bar's own length (green low, yellow mid, red only at the
 * top end) — not by the level itself, so the color transition point is
 * fixed regardless of how loud the track is, matching a real VU meter.
 *
 * *level is persistent smoothing state (one double per meter, owned by the
 * caller and passed in every call) — a real VU meter's needle has ballistic
 * damping (fast rise, slow fall, ~300ms time constant) instead of jumping
 * to the instantaneous sample peak every redraw; reading raw peaks with no
 * smoothing read as "unrealistically fast" jitter (user feedback on the
 * first version of this meter) rather than a natural meter movement. */
static void draw_vu_horizontal(FBDev *fb, const int16_t *samples, int count,
                                int x0, int y, int w, int height, double *level)
{
    fb_fill_rect_alpha(fb, x0, y, w, height, 0x30, 0x30, 0x30, 255);
    if (w <= 0) return;

    /* count==0 (e.g. paused — see draw_now_playing) falls straight through
     * with peak left at 0, so the meter decays to empty via the normal
     * smoothing below instead of freezing on the last real reading. */
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int v = samples[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    double raw = peak / 32768.0;
    if (raw > 1.0) raw = 1.0;
    const double attack = 0.5, decay = 0.06;   /* jump up fast, settle back down slowly */
    *level += (raw - *level) * (raw > *level ? attack : decay);
    int filled = (int)(*level * w);

    int green_w  = w * 60 / 100;
    int yellow_w = w * 25 / 100;   /* zones: 0-60% green, 60-85% yellow, 85-100% red */

    int seg = filled < green_w ? filled : green_w;
    if (seg > 0) fb_fill_rect_alpha(fb, x0, y, seg, height, 0x30, 0xE0, 0x30, 255);

    if (filled > green_w) {
        seg = filled - green_w;
        if (seg > yellow_w) seg = yellow_w;
        fb_fill_rect_alpha(fb, x0 + green_w, y, seg, height, 0xE0, 0xC0, 0x20, 255);
    }
    if (filled > green_w + yellow_w) {
        seg = filled - green_w - yellow_w;
        fb_fill_rect_alpha(fb, x0 + green_w + yellow_w, y, seg, height, 0xE0, 0x30, 0x30, 255);
    }
}

static void draw_now_playing(FBDev *fb, JfItem *it, double pos)
{
    fb_clear(fb);
    if      (g_now_playing_bg == 1) draw_rain(fb);
    else if (g_now_playing_bg == 2) { draw_toasty_bg(fb); draw_now_playing_gradient(fb); }
    else                            draw_starfield(fb);

    draw_clock(fb);

    if (now_sec() < g_now_playing_bg_shown_until)
        draw_text(fb, SAFE_X, SAFE_Y, NOW_PLAYING_BG_NAMES[g_now_playing_bg], 1, COL_HINT);
    if (g_paused)
        draw_text(fb, SAFE_X, SAFE_Y + 10, "PAUSED", 1, COL_RESUME);

    const int cover_max = 165;
    int cover_top = SAFE_Y;
    int cy = cover_top + cover_max / 2;

    /* No placeholder box when there's genuinely no cover (track has no
     * embedded art and no album fallback either, see JfItem.image_tag) —
     * per user request, empty space reads better than a gray rectangle
     * that looks like a broken image. */
    if (g_nowplaying_cover_px)
        blit_fit_centered(fb, g_nowplaying_cover_px, g_nowplaying_cover_w, g_nowplaying_cover_h,
                           fb->width / 2, cy, cover_max, cover_max, 255);

    int16_t af_buf[4096];
    /* Don't read the export file while paused — decode has stopped, so it
     * would just keep returning the same stale pre-pause samples, reading
     * as a frozen meter (user feedback) instead of settling to empty. */
    int af_n = g_paused ? 0 : read_af_samples(af_buf, 4096);

    int ty = cover_top + cover_max - 3;
    draw_text(fb, (fb->width - text_width(it->name, 1)) / 2, ty, it->name, 1, COL_TITLE);
    ty += 10;

    char sub[300] = {0};
    if (it->artist[0] && it->album[0])
        snprintf(sub, sizeof(sub), "%s - %s", it->artist, it->album);
    else if (it->artist[0])
        snprintf(sub, sizeof(sub), "%s", it->artist);
    else if (it->album[0])
        snprintf(sub, sizeof(sub), "%s", it->album);
    if (sub[0]) {
        truncate_to_width(sub, 1, fb->width - 2 * SAFE_X);
        draw_text(fb, (fb->width - text_width(sub, 1)) / 2, ty, sub, 1, COL_ITEM);
    }
    ty += 16;   /* clearer gap so the bar itself visibly sits between the album line and its own time label below */

    draw_timeline(fb, ty, pos, (double)it->runtime_ticks / 10000000.0);
    ty += 28;   /* centers the VU meter pair in the gap down to the hint line below, not right under the timeline */

    /* Classic L/R VU meters, full width, stacked — between the seek/timeline
     * line and the control hints below. g_vu_level_l/r are persistent
     * attack/decay state, see draw_vu_horizontal. */
    int vu_w = fb->width - 2 * SAFE_X;
    int vu_h = 4, vu_gap = 4;
    draw_vu_horizontal(fb, af_buf, af_n / 2, SAFE_X, ty, vu_w, vu_h, &g_vu_level_l);
    ty += vu_h + vu_gap;
    draw_vu_horizontal(fb, af_buf + af_n / 2, af_n - af_n / 2, SAFE_X, ty, vu_w, vu_h, &g_vu_level_r);

    /* Hint line stays at the SAME height as every other screen's hint row
     * (fb->height - 8 - SAFE_Y_BOT) — everything above it got tightened/
     * moved up instead, per user feedback that this must stay consistent. */
    const char *hint = g_paused ? "A:resume  L/R:seek  U/D:prev/next  SELECT:bg  B:stop"
                                 : "A:pause  L/R:seek  U/D:prev/next  SELECT:bg  B:stop";
    draw_text(fb, (fb->width - text_width(hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    /* Mega-tier Toasty sprites fly over everything above — see
     * draw_toasty_fg()'s own comment. */
    if (g_now_playing_bg == 2) draw_toasty_fg(fb);

    fb_flip(fb);
}

/* ── main ────────────────────────────────────────────────────────────────── */

/* Hidden dev tool, not part of the shipped app UI: renders the About
 * screen's starfield (minus the version/hint footer) to a sequence of raw
 * BGRA framebuffer dumps under /tmp, for tools/capture_about_gif.py to turn
 * into a marketing GIF for the repo's README. Doesn't touch the real
 * running app state at all — just fb_open + draw + dump + repeat. */
static int run_capture_about(int frame_count)
{
    srand(1);
    FBDev fb;
    if (fb_open(&fb, "/dev/fb0") < 0) {
        fprintf(stderr, "Cannot open /dev/fb0\n");
        return 1;
    }
    for (int i = 0; i < frame_count; i++) {
        draw_about_frame(&fb, 0);
        char path[64];
        snprintf(path, sizeof(path), "/tmp/about_frame_%03d.raw", i);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(fb.mem, 1, (size_t)fb.stride * fb.height, f);
            fclose(f);
        }
        usleep(40000);
    }
    printf("%d %d %d\n", fb.width, fb.height, fb.stride);   /* for the GIF assembler */
    fb_close(&fb);
    return 0;
}

/* Hidden dev tool for iterating on browse-screen layout (the carousel, in
 * particular) without a real /dev/fb0 or deploying to hardware each time —
 * fabricates an FBDev backed by plain malloc'd buffers (fb_open needs a
 * real fbdev ioctl, which this desktop build doesn't have) and renders one
 * real draw_browse() frame against the live server from jellyfin.conf (or
 * ./jellyfin.conf — see jf_config_load), then dumps the back-buffer as a
 * raw BGRX file for tools/raw_to_png.py to turn into something viewable.
 * Optional argv[2] pre-selects g_sel so a specific card can be previewed
 * as the active one. */
static int run_preview_browse(int sel, int list_mode)
{
    srand((unsigned)time(NULL));   /* grid_cell_order_shuffle draws on rand() */
    FBDev fb = {0};
    fb.width = 640; fb.height = 288; fb.stride = fb.width * 4;
    fb.mmap_size = (size_t)fb.stride * fb.height;
    fb.mem  = calloc(1, fb.mmap_size);
    fb.back = calloc(1, fb.mmap_size);
    if (!fb.mem || !fb.back) { fprintf(stderr, "alloc failed\n"); return 1; }

    if (!jf_config_load(&g_cfg)) { fprintf(stderr, "jellyfin.conf not found\n"); return 1; }
    if (jf_resolve_user_id(&g_cfg) != 1) { fprintf(stderr, "user resolve failed\n"); return 1; }

    g_root_list_mode = list_mode;
    push_frame(FRAME_VIEWS, "MiSTerFin", NULL, NULL, NULL);
    if (sel >= 0 && sel < g_item_count) g_sel = sel;

    draw_browse(&fb);

    FILE *f = fopen("/tmp/preview.raw", "wb");
    if (f) { fwrite(fb.mem, 1, fb.mmap_size, f); fclose(f); }
    printf("%d %d %d\n", fb.width, fb.height, fb.stride);
    return 0;
}

/* Restores whatever screen was behind the About overlay once it closes —
 * previously this only handled STATE_BROWSE, so closing About from the info
 * screen or the now-playing screen left the last About frame frozen on
 * screen instead of actually going back. */
static void redraw_current_screen(FBDev *fb, AppState state)
{
    switch (state) {
    case STATE_BROWSE:       draw_browse(fb); break;
    case STATE_INFO:         draw_info(fb); break;
    case STATE_PLAYING_AUDIO: draw_now_playing(fb, &g_items[g_audio_queue_pos], play_position()); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--capture-about") == 0)
        return run_capture_about(argc > 2 ? atoi(argv[2]) : 60);
    if (argc > 1 && strcmp(argv[1], "--preview-browse") == 0)
        return run_preview_browse(argc > 2 ? atoi(argv[2]) : -1,
                                   argc > 3 && strcmp(argv[3], "list") == 0);

    srand((unsigned)time(NULL));   /* for the About screen's starfield */

    signal(SIGTERM,  on_signal);
    signal(SIGINT,   on_signal);
    signal(SIGPIPE,  SIG_IGN);
    signal(SIGSEGV,  on_fatal);
    signal(SIGABRT,  on_fatal);
    signal(SIGBUS,   on_fatal);

    FBDev fb;
    if (fb_open(&fb, "/dev/fb0") < 0) {
        fprintf(stderr, "Cannot open /dev/fb0\n");
        return 1;
    }
    memcpy(fb.mem, fb.back, (size_t)fb.stride * fb.height);

    cursor_hide();
    input_open();
    input_drain();

    /* Enable vsync by default — mplayer's patched vo_fbdev checks this file
     * each frame (see VSYNC_FLAG comment above). */
    { int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644); if (vf >= 0) close(vf); }

    AppState state;
    if (!jf_config_load(&g_cfg)) {
        state = STATE_CONFIG_ERROR;
        snprintf(g_setup_reason, sizeof(g_setup_reason), "jellyfin.conf not found or incomplete");
        draw_setup_screen(&fb, g_setup_reason);
    } else {
        int resolved = jf_resolve_user_id(&g_cfg);
        if (resolved == 1) {
            state = STATE_BROWSE;
            push_frame(FRAME_VIEWS, "MiSTerFin", NULL, NULL, NULL);
        } else {
            state = STATE_CONFIG_ERROR;
            /* -1 = the request itself failed (server unreachable) — don't
             * blame the config for that, it might be perfectly correct and
             * the server's just down/wrong URL. 0 = server answered but no
             * user matched, which really is a config problem. */
            snprintf(g_setup_reason, sizeof(g_setup_reason),
                     resolved == -1 ? "Can't connect to server (check server URL)"
                                    : "Username not found on server (check spelling)");
            draw_setup_screen(&fb, g_setup_reason);
        }
    }

    /* Enable DDR native-video only when menu_zaparoo.rbf is the active menu core
     * (same guard MiSTerDVD uses — running the DDR copy loop against standard
     * menu.rbf adds bus contention without benefit). */
    {
        struct stat mst;
        int zaparoo_active = (stat("/media/fat/menu.rbf", &mst) == 0 &&
                              mst.st_size == 2513448);
        if (zaparoo_active && ddr_init() == 0)
            ddr_set_mode(strcasecmp(g_cfg.tv_mode, "NTSC") == 0 ? 0 : 2);
    }

    /* Start GitHub release check in background — result appears on the
     * About screen once it lands. */
    {
        pthread_t upd_tid;
        pthread_attr_t upd_attr;
        pthread_attr_init(&upd_attr);
        pthread_attr_setdetachstate(&upd_attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&upd_tid, &upd_attr, update_check_thread, NULL);
        pthread_attr_destroy(&upd_attr);
    }

    int playing = 0;
    int spinner_frame_ctr = 0;
    int about_visible = 0;
    double last_about_press = 0.0;
    if (state == STATE_BROWSE) draw_browse(&fb);

    while (g_running) {
        int inp = input_poll();
        double loop_now = now_sec();

        /* START toggles the About screen — only from the browser, not
         * mid-playback (same guard MiSTerDVD uses). */
        if (!playing && (inp & INP_START) && (loop_now - last_about_press > 0.3)) {
            last_about_press = loop_now;
            about_visible = !about_visible;
            if (about_visible) draw_about(&fb);
            else redraw_current_screen(&fb, state);
            input_drain();
            continue;
        }
        if (about_visible) {
            if (inp & INP_B) {
                about_visible = 0;
                redraw_current_screen(&fb, state);
            } else {
                draw_about(&fb);   /* redraw every frame to pick up update state */
            }
            usleep(16000);
            continue;
        }

        if (g_submenu_visible) {
            static double last_nav_press = 0.0;
            int n_opts = g_info_item.sub_count + 1;
            if ((inp & (INP_UP | INP_DOWN | INP_LEFT | INP_RIGHT)) &&
                loop_now - last_nav_press > 0.15) {
                last_nav_press = loop_now;
                if (inp & INP_UP)    { if (g_submenu_sel > 0) g_submenu_sel--; }
                if (inp & INP_DOWN)  { if (g_submenu_sel < n_opts - 1) g_submenu_sel++; }
                /* Live-tunable on top of the fixed baseline (AUDIO_DELAY_SEC
                 * + SUBTITLE_SYNC_FUDGE_SEC + g_play_offset) — for whatever
                 * that fixed default doesn't cover on a specific subtitle
                 * file. Applies immediately if a subtitle is already
                 * loaded. */
                if (inp & INP_LEFT)  { g_sub_delay_extra -= 0.1; sub_delay_send(); }
                if (inp & INP_RIGHT) { g_sub_delay_extra += 0.1; sub_delay_send(); }
            }
            if (inp & INP_A) { submenu_confirm(&fb); input_drain(); continue; }
            /* SELECT also closes — a SELECT press while already open was a
             * silent no-op before (it only opens from the STATE_PLAYING
             * switch below, which this block's own `continue` never
             * reaches while visible), so a second SELECT looked like "the
             * menu doesn't open". B still means an explicit cancel too. */
            if (inp & (INP_B | INP_SELECT)) { submenu_close(); input_drain(); continue; }
            /* Redraw every single frame, unconditionally — mplayer writes
             * video frames directly to fb->mem (bypassing our fb->back
             * entirely) and was confirmed on hardware to still do so
             * periodically even while "paused" for the overlay, silently
             * erasing this box between redraws. Only redrawing on input
             * change (an earlier version of this) meant the box could go
             * invisible while g_submenu_visible was still 1 underneath —
             * so LEFT/RIGHT looked like it should be seeking (this block
             * is exactly what intercepts LEFT/RIGHT for sync instead of
             * letting it reach the seek handler below), but the menu was
             * still silently open. Constant redraw keeps the box visibly
             * pinned the whole time it's actually open, so that state is
             * never invisible. */
            draw_submenu(&fb);
            usleep(16000);
            continue;
        }

        switch (state) {

        case STATE_CONFIG_ERROR:
            if (inp & INP_B) { g_running = 0; break; }
            draw_setup_screen(&fb, g_setup_reason);   /* redrawn every frame for the starfield */
            break;

        case STATE_BROWSE: {
            int nav = 0;
            int at_root      = (g_stack[g_stack_depth - 1].kind == FRAME_VIEWS);
            int is_carousel  = at_root && !g_root_list_mode;

            if (g_confirm_exit) {
                /* Dialog eats all other input while open. */
                if (inp & INP_A) { g_running = 0; break; }
                if (inp & INP_B) { g_confirm_exit = 0; draw_browse(&fb); input_drain(); }
                break;
            }
            if (inp & INP_B) {
                if (!pop_frame()) {
                    g_confirm_exit = 1;
                    draw_browse(&fb);
                    draw_confirm_exit(&fb);
                    input_drain();
                    break;
                }
                nav = 1;
            }
            /* SELECT swaps the root screen between the carousel and the
             * classic list, per user request — only meaningful at the root
             * (deeper frames have no second layout to switch to). */
            if (at_root && (inp & INP_SELECT)) {
                g_root_list_mode = !g_root_list_mode;
                nav = 1;
            }
            /* SELECT on the music library's artist list starts an infinite
             * shuffle across the whole library instead of drilling in. */
            if (!at_root && (inp & INP_SELECT) &&
                g_item_count > 0 && g_items[0].type == JF_TYPE_ARTIST) {
                const char *lib_id = g_stack[g_stack_depth - 1].parent_id;
                int n = jf_list_random_tracks(&g_cfg, lib_id, g_items, JF_MAX_ITEMS);
                if (n > 0) {
                    g_item_count  = n;
                    g_shuffle_mode = 1;
                    play_audio(&fb, 0);
                    state = STATE_PLAYING_AUDIO;
                }
                input_drain();
                continue;
            }
            /* Root library screen is the horizontal carousel (see
             * draw_browse_carousel) — LEFT/RIGHT move the active card
             * instead of the regular list's UP/DOWN, with a slide (see
             * carousel_slide_animate) bridging the two positions. The
             * ordinary draw_browse() call below (nav=1) still runs
             * afterwards — that's what actually switches the background
             * grid to the new library, since the slide itself deliberately
             * leaves the old one up throughout. */
            if (is_carousel) {
                if (inp & INP_LEFT && g_sel > 0) {
                    int old_sel = g_sel--;
                    carousel_slide_animate(&fb, old_sel, g_sel);
                    nav = 1;
                }
                if (inp & INP_RIGHT && g_sel < g_item_count - 1) {
                    int old_sel = g_sel++;
                    carousel_slide_animate(&fb, old_sel, g_sel);
                    nav = 1;
                }
            } else {
                if (inp & INP_UP && g_item_count > 0) {
                    if (g_sel > 0) g_sel--;
                    if (g_sel < g_scroll) g_scroll = g_sel;
                    nav = 1;
                }
                if (inp & INP_DOWN && g_item_count > 0) {
                    if (g_sel < g_item_count - 1) g_sel++;
                    if (g_sel >= g_scroll + VISIBLE) g_scroll = g_sel - VISIBLE + 1;
                    nav = 1;
                }
            }
            if (at_root) g_root_sel = g_sel;   /* see g_root_sel's own comment */
            if (inp & INP_A && g_item_count > 0) {
                JfItem *it = &g_items[g_sel];
                BrowseFrame *f = &g_stack[g_stack_depth - 1];
                switch (it->type) {
                case JF_TYPE_FOLDER:
                case JF_TYPE_ARTIST:
                    push_frame(FRAME_ITEMS, it->name, it->id, NULL, NULL);
                    nav = 1;
                    break;
                case JF_TYPE_ALBUM: {
                    /* "Artist / Album" — f->title is still the artist's own
                     * name here (that's the frame we're drilling out of),
                     * so no extra field lookup needed. */
                    char combined[128];
                    snprintf(combined, sizeof(combined), "%s / %s", f->title, it->name);
                    push_frame(FRAME_ITEMS, combined, it->id, NULL, NULL);
                    nav = 1;
                    break;
                }
                case JF_TYPE_SERIES:
                    push_frame(FRAME_SEASONS, it->name, NULL, it->id, NULL);
                    nav = 1;
                    break;
                case JF_TYPE_SEASON: {
                    /* "Series / Season", same pattern as Artist / Album. */
                    char combined[128];
                    snprintf(combined, sizeof(combined), "%s / %s", f->title, it->name);
                    push_frame(FRAME_EPISODES, combined, NULL, f->series_id, it->id);
                    nav = 1;
                    break;
                }
                case JF_TYPE_MOVIE:
                case JF_TYPE_EPISODE:
                    info_assets_load(&fb, it, &spinner_frame_ctr);
                    state = STATE_INFO;
                    draw_info(&fb);
                    input_drain();
                    break;
                case JF_TYPE_TRACK:
                    play_audio(&fb, g_sel);
                    state = STATE_PLAYING_AUDIO;
                    draw_now_playing(&fb, &g_items[g_sel], 0.0);
                    input_drain();
                    break;
                default:
                    break;
                }
            }
            if (nav && state == STATE_BROWSE) draw_browse(&fb);

            /* Keeps the top-bar clock live and the over-long-title marquee
             * (see draw_browse) crawling even with no input at all. Cheap
             * enough at this rate — other screens already redraw fully
             * every ~16ms with no issue, this is a tenth of that. */
            static double last_browse_tick = 0.0;
            if (state == STATE_BROWSE && loop_now - last_browse_tick > 0.1) {
                last_browse_tick = loop_now;
                g_marquee_px += 1.5;
                draw_browse(&fb);
            }
            break;
        }

        case STATE_INFO:
            if (inp & INP_B) {
                info_assets_free();
                state = STATE_BROWSE;
                draw_browse(&fb);
                input_drain();
            } else if (inp & INP_A) {
                double offset = (g_info_item.played) ? 0.0 :
                    (double)g_info_item.resume_ticks / 10000000.0;
                info_assets_free();
                playing = 1;
                state = STATE_PLAYING;
                g_current_sub_index = -1;
                g_burned_in_sub_index = -1;
                play(&fb, g_info_item.id, offset);
                input_drain();
            } else if ((inp & INP_SELECT) && g_info_item.resume_ticks > 0 && !g_info_item.played) {
                info_assets_free();
                playing = 1;
                state = STATE_PLAYING;
                g_current_sub_index = -1;
                g_burned_in_sub_index = -1;
                play(&fb, g_info_item.id, 0.0);
                input_drain();
            }
            break;

        case STATE_PLAYING:
            if (!player_running()) {
                jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                                   (int64_t)(play_position() * 10000000.0));
                playing = 0;
                state = STATE_BROWSE;
                draw_browse(&fb);
            } else if (inp & INP_B) {
                jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                                   (int64_t)(play_position() * 10000000.0));
                player_stop();
                playing = 0;
                state = STATE_BROWSE;
                draw_browse(&fb);
            } else if (g_paused) {
                if (inp & INP_SELECT) { submenu_open(&fb); draw_submenu(&fb); continue; }
                if (inp & INP_LEFT)  seek_accumulate(-SEEK_STEP, loop_now);
                if (inp & INP_RIGHT) seek_accumulate(+SEEK_STEP, loop_now);
                if (inp & INP_A) player_pause_toggle();
                if (inp & INP_L) {
                    int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                    if (vf >= 0) close(vf);
                    osd_flash("VSync: ON", 1);
                }
                if (inp & INP_R) {
                    unlink(VSYNC_FLAG);
                    osd_flash("VSync: OFF", 1);
                }
                /* seek_pending_target() reflects any not-yet-fired
                 * accumulated seek too, so this timeline moves live as the
                 * user taps LEFT/RIGHT instead of waiting for the debounce
                 * to actually fire the restart. */
                draw_paused(&fb, g_info_item.name, seek_pending_target());
            } else {
                if (inp & INP_SELECT) { submenu_open(&fb); draw_submenu(&fb); continue; }
                if (inp & INP_A) player_pause_toggle();
                if (inp & INP_LEFT)  seek_accumulate(-SEEK_STEP, loop_now);
                if (inp & INP_RIGHT) seek_accumulate(+SEEK_STEP, loop_now);
                if (inp & INP_L) {
                    int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                    if (vf >= 0) close(vf);
                    osd_flash("VSync: ON", 0);
                }
                if (inp & INP_R) {
                    unlink(VSYNC_FLAG);
                    osd_flash("VSync: OFF", 0);
                }

                if (loop_now - g_last_progress_report >= PROGRESS_REPORT_INTERVAL) {
                    g_last_progress_report = loop_now;
                    jf_report_progress(&g_cfg, g_info_item.id, g_play_session_id,
                                        (int64_t)(play_position() * 10000000.0), 0);
                }
            }
            break;

        case STATE_PLAYING_AUDIO: {
            JfItem *cur = &g_items[g_audio_queue_pos];
            if (!player_running()) {
                jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                   (int64_t)(play_position() * 10000000.0));
                if (g_audio_queue_pos + 1 < g_item_count &&
                    g_items[g_audio_queue_pos + 1].type == JF_TYPE_TRACK) {
                    play_audio(&fb, g_audio_queue_pos + 1);
                } else if (g_shuffle_mode) {
                    /* Ran out of the current random batch — fetch a fresh
                     * one and keep going, forever, instead of stopping. */
                    const char *lib_id = g_stack[g_stack_depth - 1].parent_id;
                    int n = jf_list_random_tracks(&g_cfg, lib_id, g_items, JF_MAX_ITEMS);
                    if (n > 0) {
                        g_item_count = n;
                        play_audio(&fb, 0);
                    } else {
                        g_shuffle_mode = 0;
                        fetch_frame();
                        state = STATE_BROWSE;
                        draw_browse(&fb);
                        break;
                    }
                } else {
                    state = STATE_BROWSE;
                    draw_browse(&fb);
                    break;
                }
            } else if (inp & INP_B) {
                jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                   (int64_t)(play_position() * 10000000.0));
                player_stop();
                if (g_shuffle_mode) { g_shuffle_mode = 0; fetch_frame(); }
                state = STATE_BROWSE;
                draw_browse(&fb);
                break;
            } else if (inp & INP_A) {
                player_pause_toggle();
            } else if (inp & INP_UP) {
                if (g_audio_queue_pos > 0 && g_items[g_audio_queue_pos - 1].type == JF_TYPE_TRACK) {
                    jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                       (int64_t)(play_position() * 10000000.0));
                    player_stop();
                    play_audio(&fb, g_audio_queue_pos - 1);
                }
            } else if (inp & INP_DOWN) {
                if (g_audio_queue_pos + 1 < g_item_count &&
                    g_items[g_audio_queue_pos + 1].type == JF_TYPE_TRACK) {
                    jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                       (int64_t)(play_position() * 10000000.0));
                    player_stop();
                    play_audio(&fb, g_audio_queue_pos + 1);
                }
            } else if (inp & INP_LEFT) {
                /* True in-place seek, not a stop+restart like video needs —
                 * confirmed on hardware that mplayer's own "seek" slave
                 * command works fine over the network for a direct-play
                 * (static=true) HTTP source, since that's a real seekable
                 * byte-range request unlike video's live transcode stream. */
                double target = play_position() - AUDIO_SEEK_STEP;
                if (target < 0) target = 0;
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "pausing_keep seek -%.1f 0\n", AUDIO_SEEK_STEP);
                mp_cmd(cmd);
                g_play_offset = target;
                g_play_start_wall = now_sec();
            } else if (inp & INP_RIGHT) {
                double dur = (double)cur->runtime_ticks / 10000000.0;
                double target = play_position() + AUDIO_SEEK_STEP;
                if (dur > 0 && target > dur) target = dur;
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "pausing_keep seek %.1f 0\n", AUDIO_SEEK_STEP);
                mp_cmd(cmd);
                g_play_offset = target;
                g_play_start_wall = now_sec();
            } else if (inp & INP_SELECT) {
                g_now_playing_bg = (g_now_playing_bg + 1) % NOW_PLAYING_BG_COUNT;
                g_now_playing_bg_shown_until = now_sec() + 1.5;
                if (g_now_playing_bg == 2 && !g_toasty_loaded) {
                    /* Draw one full frame first — cover/title/timeline/VU/
                     * hint all render normally, background stays plain
                     * black since draw_toasty_bg() no-ops until loaded —
                     * then toasty_load()'s own spinner-flip loop overlays
                     * on top of that same frame while it decodes, instead
                     * of the load being the first thing drawn this tick. */
                    draw_now_playing(&fb, cur, play_position());
                    toasty_load(&fb);
                }
            } else if (loop_now - g_last_progress_report >= PROGRESS_REPORT_INTERVAL) {
                g_last_progress_report = loop_now;
                jf_report_progress(&g_cfg, cur->id, g_play_session_id,
                                    (int64_t)(play_position() * 10000000.0), g_paused);
            }
            draw_now_playing(&fb, &g_items[g_audio_queue_pos], play_position());
            break;
        }
        }

        if (g_seek_fire_at > 0.0 && loop_now >= g_seek_fire_at && playing) {
            double accum = g_seek_accum;
            g_seek_accum = 0.0;
            g_seek_fire_at = 0.0;
            player_seek(&fb, accum);
        }

        if (playing && ddr_ready()) {
            /* Pace this to the real display refresh instead of a blind
             * 16ms software timer — sampling fb->mem (written by mplayer at
             * the source's own ~23.976fps) on an unrelated fixed tick beats
             * unevenly against the source frame rate, showing as judder
             * that's independent of mplayer's own vsync setting (confirmed:
             * toggling VSync on/off in-app didn't change it — that flag
             * only affects mplayer's write timing, not this read loop). */
            fb_wait_vsync(&fb);
            ddr_copy_from_fb(fb.mem, fb.stride);
            ddr_flip(0, 0);
        } else {
            if (!playing && ddr_ready()) ddr_stop();
            usleep(16000);
        }
    }

    player_stop();
    info_assets_free();
    ddr_close();
    cursor_show();
    fb_clear(&fb);
    fb_flip(&fb);
    fb_close(&fb);
    input_close();
    return 0;
}
