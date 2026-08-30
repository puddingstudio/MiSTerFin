#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <termios.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "fb.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ddr.h"
#include "jellyfin.h"
#include "json.h"
#include "subtitles.h"
#include "update.h"
#include "input.h"
#include "draw.h"
#include "screenshot.h"
#include "sfx.h"
#include "util.h"
#include "grid.h"
#include "visualizers.h"
#include "session.h"

#define MPLAYER      "/media/fat/misterfin/mplayer-arm"
/* See stream_via_curl_if_https — only one stream (video or music, never
 * both) plays at a time, so a single fixed path is enough. */
#define STREAM_FIFO_PATH "/tmp/misterfin_stream.fifo"
/* Main-thread download scratch file. grid.c names the same path (its
 * GRID_POSTER_TMP) for its own main-thread downloads — same thread, so
 * sharing is deliberate and safe; keep the two in sync. */
#define POSTER_TMP   "/tmp/misterfin_poster.img"
/* Per-item browse-list cover art, cached to persistent storage (same idea as
 * the home-screen grid cache above) so scrolling a list reads covers from the
 * cache instead of re-downloading each one — no network round-trip on the
 * main thread, so no scroll freeze and no blank-then-appear. A background
 * thread (cover_prefetch_thread) fills the cache for the current list;
 * COVER_TMP_BG is that thread's own download scratch path. */
/* UI click/confirm clips. Same directory screenshot.c's shutter.wav lives in,
 * and already deployed by the Makefile and copied by the in-app updater. */
#define SFX_DIR      "/media/fat/misterfin/sfx"
#define COVER_TMP_BG   "/tmp/misterfin_cover_bg.img"
#define CRASH_LOG    "/media/fat/misterfin/crash.log"
/* mplayer's -af export writes live PCM samples to this mmap'd file for the
 * now-playing visualizer to read — confirmed supported on this build
 * (some builds/filters aren't, see the mplayer/subfont -osdlevel comments
 * elsewhere in this file for other examples of that). Unlinked before each
 * track starts so a stale file from a previous track can't be misread as
 * current audio for the first frame or two. */
/* Hardcoded inside the shared mplayer-arm binary's own patched vo_fbdev.c —
 * NOT ours to rename. Its presence makes vo_fbdev wait for vblank before
 * each frame, trading a little extra decode latency for tear-free output. */
#define VSYNC_FLAG   "/tmp/misterdvd_vsync"

/* VISIBLE row count is now derived from the live framebuffer height —
 * see visible_rows() below — instead of this fixed constant, so the list
 * fits without overlap at any active-line count (was hardcoded 7, tuned
 * only for PAL's 288 lines; NTSC's 240 needs fewer rows to avoid the last
 * row colliding with the bottom hint bar). */
#define ROW_H        30
/* Broadcast "title-safe" convention (80% of frame visible, 10% margin per
 * side) — protects on-screen text from CRT overscan cropping. Computed
 * once from the live framebuffer size (see main()) instead of hardcoded,
 * so it's correct at any resolution; 24/20 below are just fallback values
 * before that computation runs. */
static int SAFE_X = 24;
static int SAFE_Y = 20;
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

volatile int g_running = 1;   /* extern-declared in input.h */
static void on_signal(int s) { (void)s; g_running = 0; }

static char g_setup_reason[64];   /* set once at startup, redrawn every frame by draw_setup_screen() */

/* Which body draw_setup_screen() shows below the reason line. A network
 * failure isn't a credential problem — telling someone mid-Quick-Connect
 * setup to go add an api_key/username is solving the wrong problem, so that
 * case gets the configured URL back instead, to check for a typo/stale IP. */
typedef enum { SETUP_HELP_CONFIG, SETUP_HELP_CONNECTION } SetupHelpKind;
static SetupHelpKind g_setup_help;   /* set alongside g_setup_reason, same lifetime */

static void cursor_show(void);  /* forward decl for emergency_cleanup */

/* ── MiSTer page-flip playback (interlaced full-frame modes) ──────────────
 * The scaler latches its framebuffer base address at vsync (ascal.vhd), so
 * repointing it is a hardware page flip — tear-free by construction, no
 * per-frame copy racing the beam. mplayer's vo_fbdev does the actual
 * per-frame flips (see docker/vo_fbdev.c, PF_* block); the app's job is
 * the choreography around a playback: create the flag file the vo checks,
 * SIGSTOP Main_MiSTer so the raw SPI flips can't race its bus traffic,
 * and on stop put the display back on page 0 (the Linux fb this app draws)
 * and wake Main back up. Engaged only under the real interlaced core
 * (fb.interlaced_core, not the broader line_double geometry — a
 * progressive HDMI canvas line-doubles the UI without any of this) —
 * progressive modes never suffered the write-vs-beam race badly enough to
 * need it, and keeping Main running is strictly safer. */
#define PAGEFLIP_FLAG "/tmp/misterfin_pageflip"
static int   g_pageflip_mode    = 0;   /* fb.interlaced_core, latched in main() */
static int   g_pageflip_engaged = 0;   /* between pageflip_begin() and _end() */
static pid_t g_mister_pid       = 0;

/* Main_MiSTer's pid, found by comm name — no shell involved (the whole
 * request pipeline is fork+execvp by design, see jf_curl_run). */
static pid_t mister_main_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    pid_t found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], comm[32];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) continue;
        comm[n] = '\0';
        if (!strcmp(comm, "MiSTer\n") || !strcmp(comm, "MiSTer"))
            found = (pid_t)atoi(e->d_name);
    }
    closedir(d);
    return found;
}

static void pageflip_begin(void)
{
    if (!g_pageflip_mode || g_pageflip_engaged) return;
    int fd = open(PAGEFLIP_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    g_mister_pid = mister_main_pid();
    if (g_mister_pid > 0) kill(g_mister_pid, SIGSTOP);
    fb_set_reflip(1);   /* app overlays re-assert page 0, see fb_set_reflip */
    g_pageflip_engaged = 1;
}

static void pageflip_end(void)
{
    if (!g_pageflip_engaged) return;
    fb_set_reflip(0);
    fb_page_flip(0);                 /* Main still stopped — SPI is safe */
    if (g_mister_pid > 0) {
        kill(g_mister_pid, SIGCONT);
        /* Main_MiSTer echoes physical joystick presses onto a synthetic
         * "MiSTer virtual input" device (see device_is_mister_virtual's
         * comment) — SIGSTOPping it for the whole playback means whatever
         * it echoes is backed up in the kernel's input event queue rather
         * than lost, and it can flush that backlog in a burst right as it
         * resumes. Confirmed on hardware: exiting video landed two screens
         * back instead of one — the backlog included an echo of the very
         * B press that stopped playback, arriving a moment later and
         * getting read as a SECOND "back" once already on the browse
         * screen. 80ms is comfortably longer than that flush takes;
         * draining after it discards the whole backlog in one place
         * rather than chasing it at every pageflip_end() call site. */
        usleep(80000);
        input_drain();
    }
    unlink(PAGEFLIP_FLAG);
    g_pageflip_engaged = 0;
}

/* ── background music suspend/resume ─────────────────────────────────────
 * A BGM script (venice1200/MiSTer_BGM's bgm.sh and its derivatives) stops
 * its own music when a CORE loads, but MiSTerFin is a script, not a core —
 * from BGM's point of view the Menu core never changed, so the music keeps
 * playing underneath it and fights for the audio device/CPU, showing up as
 * video stutter (reported in #15). Confirmed on real hardware (bgm.sh, as
 * shipped): it plays MP3 via mpg123, WAV via aplay, MIDI via aplaymidi and
 * VGM via vgmplay — there's no single player process to SIGSTOP. What it
 * does have is its own control socket, the same one its interactive "Stop
 * playing"/"Start playing" menu entries use, so this drives that instead of
 * guessing at a process name. */
#define BGM_SOCKET_PATH "/tmp/bgm.sock"

/* Connects to bgm.sh's control socket, sends cmd, and (if out is non-NULL)
 * reads its reply. Returns 0 if bgm.sh isn't running (no listener on the
 * socket) or the connection dropped, in which case *out is left untouched. */
static int bgm_send(const char *cmd, char *out, size_t outlen)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BGM_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    if (ok) {
        ssize_t sent = write(fd, cmd, strlen(cmd));
        ok = (sent == (ssize_t)strlen(cmd));
        if (ok && out && outlen > 0) {
            ssize_t n = read(fd, out, outlen - 1);
            out[n > 0 ? n : 0] = '\0';
        }
    }
    close(fd);
    return ok;
}

/* Set only once bgm_pause() has actually stopped a playing BGM — so
 * bgm_resume() never turns music back ON for someone who had it paused (or
 * had BGM's own "playback: disabled" setting) before MiSTerFin even
 * started. */
static int g_bgm_was_playing = 0;

static void bgm_pause(void)
{
    /* "status" replies "<is_playing>\t<playback>\t<playlist>\t<file>" —
     * <playback> (random/loop/disabled) is BGM's configured mode, unlike
     * <is_playing> which flips to "no" for an instant between tracks even
     * while a playlist is actively running. */
    char status[64];
    if (!bgm_send("status", status, sizeof(status))) return;
    char *playback = strchr(status, '\t');
    if (!playback || !strncmp(playback + 1, "disabled", 8)) return;

    bgm_send("stop", NULL, 0);
    g_bgm_was_playing = 1;
}

static void bgm_resume(void)
{
    if (!g_bgm_was_playing) return;
    bgm_send("play", NULL, 0);
    g_bgm_was_playing = 0;
}

static void emergency_cleanup(void)
{
    /* A crash with Main_MiSTer SIGSTOPped (or the display parked on the
     * flip page) would leave the whole device wedged — undo both first. */
    pageflip_end();
    bgm_resume();
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

/* Set from fb.headless right after fb_open — see the MISTERFIN_FB comment in
 * fb.c. Guards the handful of places that touch real console/FPGA hardware
 * with no meaningful desktop equivalent. */
static int g_headless = 0;

static void cursor_hide(void)
{
    /* Off-hardware the "console" this would grab is the developer's own
     * terminal emulator — KD_GRAPHICS is a harmless ENOTTY there, but the
     * clear-screen write is not, and it fights the raw-mode stdin backend
     * for the same tty. */
    if (g_headless) return;
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
    if (g_headless) return;   /* see cursor_hide() */
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

/* Byte-for-byte file comparison (chunked, no allocation beyond the two
 * stack buffers). Any I/O error reads as "not identical" — used for the
 * Zaparoo core detection below, where a false negative just means the
 * standard video path, never a wrong one. */
static int files_identical(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb_ = fopen(b, "rb");
    int same = (fa && fb_);
    while (same) {
        unsigned char ba[65536], bb[65536];
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb_);
        if (na != nb || memcmp(ba, bb, na) != 0) same = 0;
        else if (na < sizeof(ba)) break;   /* EOF on both, all equal */
    }
    if (fa) fclose(fa);
    if (fb_) fclose(fb_);
    return same;
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

/* ── app state ────────────────────────────────────────────────────────────── */

typedef enum {
    STATE_CONFIG_ERROR, STATE_QUICK_CONNECT,
    STATE_BROWSE, STATE_INFO, STATE_PLAYING, STATE_PLAYING_AUDIO
} AppState;
typedef enum {
    FRAME_VIEWS, FRAME_ITEMS, FRAME_SEASONS, FRAME_EPISODES,
    FRAME_RESUME,   /* Continue Watching — see JF_VIEW_RESUME */
    FRAME_NEXTUP    /* Next Up          — see JF_VIEW_NEXTUP */
} FrameKind;

/* Rows fetched for each home row. Both are "what to watch next" lists rather
 * than libraries to browse — past a couple of screenfuls nobody scrolls, so
 * they're capped instead of paginated. */
#define HOME_ROW_MAX 24

#define MAX_STACK 8

typedef struct {
    FrameKind kind;
    char      title[128];
    char      parent_id[JF_ID_LEN];  /* FRAME_ITEMS */
    char      collection_type[16];   /* FRAME_ITEMS; inherited through folders */
    char      series_id[JF_ID_LEN];  /* FRAME_SEASONS / FRAME_EPISODES */
    char      season_id[JF_ID_LEN];  /* FRAME_EPISODES */
} BrowseFrame;

static JfConfig   g_cfg;
static BrowseFrame g_stack[MAX_STACK];
static int         g_stack_depth = 0;

static JfItem g_items[JF_MAX_ITEMS];
static int    g_item_count = 0;
static int    g_sel = 0, g_scroll = 0;
/* Where the selection highlight actually IS, in visible-row units, as
 * distinct from g_sel where it is headed — see draw_browse's own comment.
 * Negative means "no position yet, put it wherever the selection is", which
 * is what a fresh frame wants: gliding in from the last screen's row would
 * read as the bar chasing the cursor rather than following it. */
static double g_sel_anim = -1.0;
/* g_items[] holds one window of the current frame's list rather than the
 * whole thing — libraries can be far larger than any buffer we'd want to
 * keep resident, and the previous fixed Limit=JF_MAX_ITEMS silently
 * presented a truncated library as if it were complete.
 *
 * g_window_start is the absolute index of g_items[0]; g_sel and g_scroll
 * stay window-relative (so every existing g_items[g_sel] use is unchanged),
 * and absolute position is g_window_start + g_sel. g_total_count is the
 * server's TotalRecordCount, or -1 when unknown — in which case the loaded
 * window is all there is, as far as anything here can tell. */
static int    g_window_start = 0;
static int64_t g_total_count = -1;
static double g_marquee_px = 0.0;         /* scroll offset for an over-long title, see draw_browse */
static char   g_marquee_title[128] = "";  /* last title drawn — reset the offset when it changes */
/* Per-library item counts for the root carousel (see draw_browse_carousel),
 * parallel to g_items[] and only meaningful while the root FRAME_VIEWS frame
 * is loaded — fetched once in fetch_frame() alongside the views themselves
 * (3 small Limit=0 count requests for this user's library, one per view).
 * -1 = fetch failed, caller just omits the count line for that card. */
static int64_t g_view_counts[JF_MAX_ITEMS];
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
 * 2 = Nebula, our audio-reactive plasma visualizer (see draw_nebula), 3 =
 * Now Spinning — the cover rendered as a spinning CD with a graphic-
 * EQ bar (see draw_now_playing_cd/draw_now_playing_eq), 4 = Toasty Squadron
 * sprites (see draw_toasty). Persists for the whole app session, same as
 * g_root_list_mode. Indices 2/3/4 are immersive modes: they hide the
 * clock/title/timeline/VU and just show an enlarged centered cover over
 * the effect (see draw_now_playing). */
#define NOW_PLAYING_BG_COUNT   6
#define NOW_PLAYING_BG_NEBULA  2
#define NOW_PLAYING_BG_SPIN    3
#define NOW_PLAYING_BG_TUNNEL  4
#define NOW_PLAYING_BG_TOASTY  5   /* last in the cycle, per user request */
static int    g_now_playing_bg = 0;
static const char *NOW_PLAYING_BG_NAMES[] = { "Starfield", "Rain", "Nebula", "Now Spinning", "Tunnel", "Toasty Squadron" };
/* Label shows briefly on change then disappears, rather than sitting on
 * screen permanently — set to now_sec()+1.5 wherever g_now_playing_bg
 * changes (see the STATE_PLAYING_AUDIO SELECT handling), 0 = not shown. */
static double g_now_playing_bg_shown_until = 0.0;

/* VizGif (the user-supplied GIF background, still implemented in
 * visualizers.c) is deliberately NOT in the cycle right now — pulled per
 * user request pending a better default GIF; these helpers are where it
 * plugs back in when it returns. */
static int now_playing_mode_count(void)
{
    return NOW_PLAYING_BG_COUNT;
}

static const char *now_playing_mode_name(int mode)
{
    return NOW_PLAYING_BG_NAMES[mode];
}
/* In the immersive backgrounds (Nebula/Toasty) the track title is otherwise
 * hidden, so on each track change it's flashed at the top for a few seconds
 * then disappears — set to now_sec()+N wherever a new track starts playing
 * (see play_audio), 0 = not shown. */
static double g_np_title_shown_until = 0.0;

static JfItem g_info_item;   /* item currently shown on the info screen */

/* ── info-screen hero assets (backdrop, logo, cast photos) ───────────────── */

#define CAST_DISPLAY_MAX 5

static uint8_t *g_backdrop_px = NULL;
static int      g_backdrop_w = 0, g_backdrop_h = 0;
static uint8_t *g_logo_px = NULL;
static int      g_logo_w = 0, g_logo_h = 0;
static uint8_t *g_cast_px[CAST_DISPLAY_MAX];
static int      g_cast_px_w[CAST_DISPLAY_MAX], g_cast_px_h[CAST_DISPLAY_MAX];


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
        /* 480, not 400: draw_info caps the drawn logo width at exactly 480
         * (see its `dw > 480` clamp), so a 400px source was being upscaled
         * on any wide logo — the one place in the UI that was asking the
         * server for less than it draws. */
        int dl_ok = jf_download_item_image(&g_cfg, g_info_item.logo_item_id, "Logo",
                                            g_info_item.logo_tag, 480, POSTER_TMP);
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

#define SPINNER_BLINK_MS 350

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
    fb_sync_back(fb);
    /* Dim the held frame while the (re)started stream buffers. Without
     * this, a seek shows the stale pre-seek frame at full strength for a
     * few seconds — including the "PAUSED" overlay text if the seek came
     * from pause — which reads as "playback stuck", reported exactly so
     * once page flipping made the held frame more visible. Dimmed, it
     * reads as the transition it actually is. */
    fb_fill_rect_alpha(fb, 0, 0, fb->width, fb->height, 0, 0, 0, 150);

    uint32_t ref[SPINNER_SAMPLE_N];
    int valid[SPINNER_SAMPLE_N];
    for (int i = 0; i < SPINNER_SAMPLE_N; i++) {
        valid[i] = SPINNER_SAMPLE_PTS[i].x < fb->width && SPINNER_SAMPLE_PTS[i].y < fb->height;
        ref[i] = valid[i] ? *(const uint32_t *)(fb_mem_row(fb, SPINNER_SAMPLE_PTS[i].y)
                                                          + SPINNER_SAMPLE_PTS[i].x * 4) : 0;
    }

    double until = now_sec() + seconds;
    int frame_idx = 0;
    while (now_sec() < until) {
        if (frame_idx > 0) {
            int changed = 0;
            for (int i = 0; i < SPINNER_SAMPLE_N && !changed; i++) {
                if (!valid[i]) continue;
                uint32_t cur = *(const uint32_t *)(fb_mem_row(fb, SPINNER_SAMPLE_PTS[i].y)
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

/* Episodes in these rows arrive out of context — the row mixes shows, so a
 * name like "Episode 03" identifies nothing. Fold the series name and season/
 * episode numbers into the display name, which keeps draw_browse unchanged
 * (it just draws whatever name it's given). The info screen re-fetches by id
 * and so still shows the clean title. */
static void home_row_label_episodes(void)
{
    for (int i = 0; i < g_item_count; i++) {
        JfItem *it = &g_items[i];
        if (it->type != JF_TYPE_EPISODE || !it->series_name[0]) continue;

        char combined[JF_NAME_LEN];
        if (it->parent_index_number > 0 && it->index_number > 0)
            snprintf(combined, sizeof(combined), "%s - S%dE%02d %s",
                     it->series_name, it->parent_index_number, it->index_number, it->name);
        else
            snprintf(combined, sizeof(combined), "%s - %s", it->series_name, it->name);

        strncpy(it->name, combined, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
    }
}

/* Loads the window of the current frame's list that starts at start_index.
 * Only the list itself moves — the frame stack, and which frame is current,
 * are untouched, so this is equally the "open this frame" path (start 0) and
 * the "scroll past the end of what's loaded" path.
 *
 * Returns 1 on success, 0 if the fetch failed — in which case the previously
 * loaded window is left intact. */
static int fetch_frame_window(int start_index)
{
    BrowseFrame *f = &g_stack[g_stack_depth - 1];
    if (start_index < 0) start_index = 0;

    /* Saved so a failed fetch can put the window back exactly as it was —
     * committing the new start and a recomputed total on failure left the
     * cursor pointing into a list that had just been emptied, which rendered
     * a perfectly healthy library as "Nothing here". */
    const int     prev_window_start = g_window_start;
    const int64_t prev_total_count  = g_total_count;
    const int     prev_item_count   = g_item_count;

    int paginated = 0;   /* only these kinds have more rows than one fetch returns */

    g_window_start = start_index;
    g_total_count  = -1;

    switch (f->kind) {
    case FRAME_VIEWS: {
        /* Library views are a handful of entries by definition — no paging. */
        g_window_start = 0;

        /* Continue Watching and Next Up go in front of the real libraries:
         * they're what someone opening the app usually wants, and putting
         * them first means the carousel starts there. Each is added only when
         * it actually has something in it — an empty "Continue Watching" card
         * is worse than no card, and both are empty on a fresh install.
         *
         * Probed with Limit=1 rather than fetched in full: all that's needed
         * here is the count for the card, and drilling in re-fetches anyway. */
        /* Card labels are short because the carousel draws them at double
         * size and clips to CAROUSEL_CARD_W — "Continue Watching" came out as
         * "Continue...". The full name is used for the frame title once you
         * drill in (see the STATE_BROWSE handler), where there's room. */
        int n_synth = 0;
        static const struct { const char *id, *name; int kind; } synth[] = {
            { JF_VIEW_RESUME, "Continue", JF_SYNTH_RESUME },
            { JF_VIEW_NEXTUP, "Next Up",  JF_SYNTH_NEXTUP },
        };
        for (int s = 0; s < 2; s++) {
            JfItem probe[1];
            int64_t total = 0;
            int got = (s == 0) ? jf_list_resume(&g_cfg, probe, 1, &total)
                                : jf_list_nextup(&g_cfg, probe, 1, &total);
            if (got <= 0) continue;
            if (total <= 0) total = got;

            JfItem *card = &g_items[n_synth];
            memset(card, 0, sizeof(*card));
            strncpy(card->id,   synth[s].id,   sizeof(card->id) - 1);
            strncpy(card->name, synth[s].name, sizeof(card->name) - 1);
            card->type = JF_TYPE_FOLDER;
            card->synthetic = synth[s].kind;
            card->index_number = -1;
            g_view_counts[n_synth] = total;
            n_synth++;
        }

        int n_views = jf_list_views(&g_cfg, g_items + n_synth, JF_MAX_ITEMS - n_synth);
        for (int i = 0; i < n_views; i++) {
            JfItem *v = &g_items[n_synth + i];
            g_view_counts[n_synth + i] =
                jf_count_items(&g_cfg, v->id, collection_item_type(v->collection_type));
        }
        g_item_count = n_synth + n_views;
        break;
    }
    case FRAME_RESUME:
        g_window_start = 0;
        g_item_count = jf_list_resume(&g_cfg, g_items, HOME_ROW_MAX, &g_total_count);
        home_row_label_episodes();
        break;
    case FRAME_NEXTUP:
        g_window_start = 0;
        g_item_count = jf_list_nextup(&g_cfg, g_items, HOME_ROW_MAX, &g_total_count);
        home_row_label_episodes();
        break;
    case FRAME_ITEMS:
        /* Episode counts for series rows used to be fetched here with one
         * extra recursive count request PER ROW — so opening a 200-series
         * TV library meant 200 sequential curl invocations before anything
         * could be drawn. They now ride along on the listing itself as
         * RecursiveItemCount, which the server computes for the whole
         * result set in a single batched query. See JfItem's own comment. */
        paginated = 1;
        g_item_count = jf_list_items(&g_cfg, f->parent_id, f->collection_type, start_index,
                                      g_items, JF_PAGE_SIZE, &g_total_count);
        break;
    case FRAME_SEASONS:
        g_window_start = 0;   /* a series' season list is always short */
        g_item_count = jf_list_seasons(&g_cfg, f->series_id, g_items, JF_MAX_ITEMS);
        break;
    case FRAME_EPISODES:
        paginated = 1;
        g_item_count = jf_list_episodes(&g_cfg, f->series_id, f->season_id, start_index,
                                         g_items, JF_PAGE_SIZE, &g_total_count);
        break;
    }
    if (g_item_count < 0) {
        /* The request failed (as opposed to legitimately returning nothing).
         * Restore the window we already had rather than commit an empty one. */
        g_window_start = prev_window_start;
        g_total_count  = prev_total_count;
        g_item_count   = prev_item_count;
        return 0;
    }

    /* For everything that isn't paginated, what's loaded IS the whole list —
     * overwrite rather than fall back. The home rows ask the server for a
     * total and then cap the fetch at HOME_ROW_MAX, so on an account with
     * more than that the total legitimately exceeds what's loaded; leaving it
     * in place made browse_move_sel think there was another page, and it
     * re-fetched the same rows on every keypress. With auto-repeat that is a
     * blocking HTTP request every 45ms for as long as DOWN is held. */
    if (!paginated || g_total_count < 0)
        g_total_count = g_window_start + g_item_count;
    return 1;
}

/* Bumped every time the browse frame's item list changes, so an in-flight
 * cover prefetch for the previous list stops instead of caching covers no
 * longer on screen. */
static volatile int g_cover_gen = 0;
static void start_cover_prefetch(void);   /* defined with the cover cache below */

static void fetch_frame(void)
{
    fetch_frame_window(0);
    g_cover_gen++;                 /* invalidate any prefetch for the old list */
    BrowseFrame *f = &g_stack[g_stack_depth - 1];
    g_sel = 0; g_scroll = 0;
    g_sel_anim = -1.0;             /* new list — place the bar, don't glide to it */
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

    /* Fill the SD cover cache for this list in the background so scrolling
     * reads covers from the persistent cache, never from the network. */
    start_cover_prefetch();
}

static void push_frame(FrameKind kind, const char *title,
                        const char *parent_id, const char *series_id, const char *season_id,
                        const char *collection_type)
{
    if (g_stack_depth >= MAX_STACK) return;
    BrowseFrame *f = &g_stack[g_stack_depth++];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    strncpy(f->title, title, sizeof(f->title) - 1);
    if (parent_id) strncpy(f->parent_id, parent_id, sizeof(f->parent_id) - 1);
    if (series_id) strncpy(f->series_id, series_id, sizeof(f->series_id) - 1);
    if (season_id) strncpy(f->season_id, season_id, sizeof(f->season_id) - 1);
    if (collection_type)
        strncpy(f->collection_type, collection_type, sizeof(f->collection_type) - 1);
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

/* How many list rows fit between the list's top (SAFE_Y + 24, see
 * draw_browse) and the bottom hint bar (fb->height - 8 - SAFE_Y_BOT)
 * without overlap. */
static int visible_rows(FBDev *fb)
{
    /* The "28" here used to be a hardcoded stand-in for 8+SAFE_Y_BOT (only
     * correct back when SAFE_Y_BOT was a flat 20) — now that SAFE_Y_BOT is
     * derived from par_correction() and comes out smaller on both PAL and
     * NTSC, using it directly here reclaims that space as an extra visible
     * row instead of leaving it as unused slack. */
    return (fb->height - (8 + SAFE_Y_BOT) - (SAFE_Y + 24)) / ROW_H;
}

/* Moves the browse-list cursor by delta rows, clamping to the list and
 * pulling the scroll window along with it. Single-stepping (UP/DOWN) and
 * whole-page jumps (LEFT/RIGHT, L/R) are the same operation with a different
 * delta, so they share this rather than each maintaining g_scroll their own
 * way. Returns 1 if anything actually moved.
 *
 * Movement is computed in ABSOLUTE list coordinates, so it doesn't care
 * whether the destination happens to be in the currently loaded window — if
 * it isn't, the window is re-fetched around it first. Windows are aligned to
 * JF_PAGE_SIZE boundaries rather than centred on the cursor so that scrolling
 * back and forth across one boundary can't thrash a re-fetch per row. */
static int browse_move_sel(FBDev *fb, int delta)
{
    if (delta == 0) return 0;

    /* Clamped on ingest: TotalRecordCount is server-supplied, and an absurd
     * value would otherwise reach the signed arithmetic below. */
    if (g_total_count > JF_MAX_TOTAL_ITEMS) g_total_count = JF_MAX_TOTAL_ITEMS;
    int total = (int)g_total_count;
    if (total <= 0) total = g_item_count;
    if (total <= 0) return 0;

    int abs_sel = g_window_start + g_sel;
    int new_abs = abs_sel + delta;
    if (new_abs < 0) new_abs = 0;
    if (new_abs > total - 1) new_abs = total - 1;
    if (new_abs == abs_sel) return 0;

    if (new_abs < g_window_start || new_abs >= g_window_start + g_item_count) {
        int target_start = (new_abs / JF_PAGE_SIZE) * JF_PAGE_SIZE;

        /* Only fetch if that's genuinely a different window. The destination
         * can land outside the loaded rows while still belonging to the page
         * we already have — a list shorter than JF_PAGE_SIZE whose reported
         * total is larger does exactly that at its last row. Re-fetching there
         * meant one blocking request per keypress, and with auto-repeat, per
         * repeat tick. */
        if (target_start != g_window_start) {
            if (!fetch_frame_window(target_start)) return 0;   /* window left intact */
            g_scroll = 0;   /* old window's scroll offset means nothing now */
        }

        if (g_item_count <= 0) return 0;
        if (new_abs >= g_window_start + g_item_count)
            new_abs = g_window_start + g_item_count - 1;
        if (new_abs < g_window_start) new_abs = g_window_start;
        if (new_abs == abs_sel) return 0;   /* clamped back to where we started */
    }

    g_sel = new_abs - g_window_start;

    int vis = visible_rows(fb);
    if (g_sel < g_scroll)              g_scroll = g_sel;
    if (g_sel >= g_scroll + vis)       g_scroll = g_sel - vis + 1;
    if (g_scroll > g_item_count - vis) g_scroll = g_item_count - vis;
    if (g_scroll < 0)                  g_scroll = 0;

    return 1;
}

/* Re-pull the current frame from the server while keeping the cursor where it
 * was (same window, same absolute row), rather than snapping to the top the
 * way a bare fetch_frame() does. Used on return from playback so a just-
 * watched item's freshly-reported watched/resume state shows immediately —
 * and, importantly, so the server-driven Continue Watching / Next Up rows
 * drop or advance the item the way the server now sees it, instead of keeping
 * a stale local copy. A failed re-fetch leaves the current window untouched. */
static void refetch_frame_keep_selection(FBDev *fb)
{
    int abs_sel = g_window_start + g_sel;
    if (!fetch_frame_window(g_window_start)) return;
    int sel = abs_sel - g_window_start;
    if (sel >= g_item_count) sel = g_item_count - 1;
    if (sel < 0) sel = 0;
    g_sel = sel;

    int vis = visible_rows(fb);
    g_scroll = (g_sel >= vis) ? g_sel - vis + 1 : 0;
    if (vis > 0 && g_scroll > g_item_count - vis) g_scroll = g_item_count - vis;
    if (g_scroll < 0) g_scroll = 0;
}


/* show_footer=0 skips the "<version> installed"/update-available line and
 * the "A: back" hint — used by the --capture-about tool to render a clean
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
        img_w_box = (int)((double)img_w / img_h * img_h_box * par_correction(fb) + 0.5);
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

    draw_text(fb, (fb->width - text_width(fb, title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp;
    draw_text(fb, (fb->width - text_width(fb, line1, s1)) / 2, cur_y, line1, s1, COL_ITEM);
    cur_y += sch + lsp;
    draw_text(fb, (fb->width - text_width(fb, line2, s1)) / 2, cur_y, line2, s1, COL_RESUME);

    if (show_footer) {
        UpdateState  us;
        InstallState is;
        char latest[32];
        update_get_state(&us, &is, latest, sizeof(latest));

        /* Same bottom margin as everything else now (SAFE_Y_BOT == SAFE_Y) —
         * not the taller margin an earlier version of this screen used. */
        int safe_y = fb->height - 8 - SAFE_Y_BOT;
        char installed[48];
        snprintf(installed, sizeof(installed), "%s installed", APP_VERSION);

        if (is == INST_DOWNLOADING) {
            static int dot_frame = 0;
            dot_frame++;
            const char *dots = (dot_frame / 20 % 3 == 0) ? "." : (dot_frame / 20 % 3 == 1) ? ".." : "...";
            char dl[48];
            snprintf(dl, sizeof(dl), "downloading %s", dots);
            draw_text(fb, SAFE_X, safe_y - 14, installed, 1, COL_DIM);
            draw_text(fb, SAFE_X, safe_y, dl, 1, 80, 180, 80);
        } else if (is == INST_DONE) {
            draw_text(fb, SAFE_X, safe_y - 14, installed, 1, COL_DIM);
            draw_text(fb, SAFE_X, safe_y, "update installed   restart app to apply", 1, 80, 180, 80);
        } else if (is == INST_FAILED) {
            draw_text(fb, SAFE_X, safe_y - 14, installed, 1, COL_DIM);
            draw_text(fb, SAFE_X, safe_y, "download failed", 1, 200, 60, 60);
        } else if (us == UPD_AVAILABLE) {
            draw_text(fb, SAFE_X, safe_y - 14, installed, 1, COL_DIM);
            char upd[64];
            snprintf(upd, sizeof(upd), "%s available   B: update", latest);
            draw_text(fb, SAFE_X, safe_y, upd, 1, 220, 150, 40);
        } else {
            draw_text(fb, SAFE_X, safe_y, installed, 1, COL_DIM);
        }

        const char *hint = "A: back";
        draw_text(fb, fb->width - text_width(fb, hint, 1) - SAFE_X, safe_y, hint, 1, COL_HINT);
    }

    fb_flip(fb);
}

static void draw_about(FBDev *fb) { draw_about_frame(fb, 1); }

/* ── what's-new screen (About -> install) ─────────────────────────────────
 * Pressing install on the About screen first shows the found release's
 * GitHub notes, scrollable, with the actual install behind one more
 * confirm — so an update is never a blind "something will change". The
 * notes arrive as markdown; this renders them as plain wrapped text with
 * a light cleanup (headers keep their own color, link targets and
 * bold/backtick markers are dropped) rather than attempting real
 * markdown. */

static void draw_top_bar(FBDev *fb, const char *title);   /* defined below with the browse UI */

#define CL_MAX_LINES 300
#define CL_LINE_H    11
static char    g_cl_lines[CL_MAX_LINES][200];
static uint8_t g_cl_hdr[CL_MAX_LINES];
static int     g_cl_count;

static void cl_push(const char *s, int hdr)
{
    if (g_cl_count >= CL_MAX_LINES) return;
    snprintf(g_cl_lines[g_cl_count], sizeof(g_cl_lines[0]), "%s", s);
    g_cl_hdr[g_cl_count] = (uint8_t)hdr;
    g_cl_count++;
}

/* Strips the markdown that would read as noise on a 640-wide text screen:
 * "#" header markers (the header-ness survives as a color), "**"/"`"
 * emphasis markers, and link targets ("[text](url)" keeps just text). */
static void cl_clean_line(const char *in, char *out, size_t outsz, int *is_hdr)
{
    *is_hdr = 0;
    while (*in == ' ') in++;
    if (*in == '#') {
        *is_hdr = 1;
        while (*in == '#') in++;
        while (*in == ' ') in++;
    }
    size_t j = 0;
    for (const char *p = in; *p && j < outsz - 3; ) {
        if (*p == '\r')                { p++; continue; }
        if (*p == '*')                 { p++; continue; }   /* *emphasis* / **bold** markers */
        if (*p == '`')                 { p++; continue; }
        if (*p == '[') {
            const char *close = strchr(p, ']');
            if (close && close[1] == '(') {
                const char *paren = strchr(close + 2, ')');
                if (paren) {
                    for (const char *q = p + 1; q < close && j < outsz - 3; q++) out[j++] = *q;
                    p = paren + 1;
                    continue;
                }
            }
        }
        /* The 8x8 font covers Latin-1 only — transliterate the typographic
         * punctuation GitHub release prose actually uses so it doesn't
         * render as '?'. */
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80) {
            unsigned char c3 = (unsigned char)p[2];
            if (c3 == 0x94 || c3 == 0x93) { out[j++] = '-'; p += 3; continue; }   /* — – */
            if (c3 == 0x98 || c3 == 0x99) { out[j++] = '\''; p += 3; continue; }  /* ' ' */
            if (c3 == 0x9C || c3 == 0x9D) { out[j++] = '"'; p += 3; continue; }   /* " " */
            if (c3 == 0xA6) { out[j++]='.'; out[j++]='.'; out[j++]='.'; p += 3; continue; }  /* … */
        }
        out[j++] = *p++;
    }
    out[j] = '\0';
}

/* Greedy word wrap of one cleaned source line into display lines; bullet
 * continuations get a two-space hang so wrapped bullets still read as one
 * item. */
static void cl_wrap_line(FBDev *fb, const char *text, int is_hdr, int max_w)
{
    if (!text[0]) { cl_push("", 0); return; }
    const char *hang = (text[0] == '-' && text[1] == ' ') ? "  " : "";

    char line[200] = "";
    const char *w = text;
    while (*w) {
        const char *e = w;
        while (*e && *e != ' ') e++;

        char cand[200];
        snprintf(cand, sizeof(cand), "%s%s%.*s", line, line[0] ? " " : "", (int)(e - w), w);
        if (line[0] && text_width(fb, cand, 1) > max_w) {
            cl_push(line, is_hdr);
            snprintf(line, sizeof(line), "%s%.*s", hang, (int)(e - w), w);
        } else {
            snprintf(line, sizeof(line), "%s", cand);
        }
        w = *e ? e + 1 : e;
    }
    if (line[0]) cl_push(line, is_hdr);
}

static void changelog_prepare(FBDev *fb)
{
    g_cl_count = 0;
    char body[8192];
    update_get_changelog(body, sizeof(body));

    int max_w = fb->width - 2 * SAFE_X;
    char *p = body;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char cleaned[600];
        int  is_hdr;
        cl_clean_line(p, cleaned, sizeof(cleaned), &is_hdr);
        cl_wrap_line(fb, cleaned, is_hdr, max_w);
        p = nl ? nl + 1 : NULL;
    }
    /* Trim trailing blank lines so max-scroll lands on real content. */
    while (g_cl_count > 0 && !g_cl_lines[g_cl_count - 1][0]) g_cl_count--;
    if (g_cl_count == 0) cl_push("(no release notes)", 0);
}

static int changelog_rows(FBDev *fb)
{
    int top    = SAFE_Y + 24;
    int bottom = fb->height - 8 - SAFE_Y_BOT - 14;
    return (bottom - top) / CL_LINE_H;
}

static void draw_changelog(FBDev *fb, int scroll)
{
    char latest[32];
    update_get_state(NULL, NULL, latest, sizeof(latest));

    fb_clear(fb);
    draw_fireworks(fb);   /* release notes get fireworks — it's a celebration, after all */
    char title[64];
    snprintf(title, sizeof(title), "What's new in %s", latest);
    draw_top_bar(fb, title);

    int rows = changelog_rows(fb);
    int y = SAFE_Y + 24;
    for (int i = scroll; i < g_cl_count && i < scroll + rows; i++) {
        if (!g_cl_lines[i][0]) { y += CL_LINE_H; continue; }
        if (g_cl_hdr[i]) draw_text(fb, SAFE_X, y, g_cl_lines[i], 1, COL_TITLE);
        else             draw_text(fb, SAFE_X, y, g_cl_lines[i], 1, COL_ITEM);
        y += CL_LINE_H;
    }

    int safe_y = fb->height - 8 - SAFE_Y_BOT;
    draw_text(fb, SAFE_X, safe_y, "A: back", 1, COL_HINT);
    if (g_cl_count > rows) {
        const char *scr = "UP/DOWN: scroll";
        draw_text(fb, (fb->width - text_width(fb, scr, 1)) / 2, safe_y, scr, 1, COL_HINT);
    }
    const char *inst = "B: install update";
    draw_text(fb, fb->width - text_width(fb, inst, 1) - SAFE_X, safe_y, inst, 1, 220, 150, 40);

    fb_flip(fb);
}

/* Shown instead of the plain black config-error screen when jellyfin.conf
 * is missing/invalid or the configured username can't be resolved — same
 * starfield/cover-art chrome as the About screen (so it's not a jarring,
 * differently-styled dead end), with the About screen's own description
 * text swapped out for setup instructions. */
/* True when the server is https:// but TLS verification hasn't been opted
 * out of — so a connection failure might really be a self-signed cert the
 * user needs to allow with INSECURE_TLS (see jf_add_tls_args). Empty/http
 * servers return 0, so the hint only appears where it can actually apply. */
static int server_https_needs_insecure_hint(void)
{
    return !g_cfg.insecure_tls && strncasecmp(g_cfg.server, "https", 5) == 0;
}

static void draw_setup_screen(FBDev *fb, const char *reason, SetupHelpKind help)
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

    int img_h_box = 0, img_w_box = 0;
    if (img_px && img_w > 0 && img_h > 0) {
        int max_w = fb->width - 2 * SAFE_X;
        /* Solved from the space available down to the hint bar (title +
         * reason + the 4-line jellyfin.conf instructions below are fixed-
         * height text that doesn't shrink with resolution), capped at 110
         * (the original PAL-tuned size) so this is a no-op at 288 active
         * lines. Without it, this screen's text overlapped the hint row on
         * NTSC's 240 lines the same way draw_info's cast row once did. */
        int max_h = fb->height - 156;
        if (max_h > 110) max_h = 110;
        if (max_h < 50) max_h = 50;
        img_h_box = max_h < img_h ? max_h : img_h;
        img_w_box = (int)((double)img_w / img_h * img_h_box * par_correction(fb) + 0.5);
        if (img_w_box > max_w) { img_h_box = img_h_box * max_w / img_w_box; img_w_box = max_w; }
    }

    int cur_y = SAFE_Y;
    /* The connection-check body below is half the lines of the config-
     * instructions one, so top-anchoring it the same way (as tuned for the
     * longer body, above) left everything bunched up with a big empty gap
     * before the fixed "A: exit" hint. Center the whole block — image,
     * title, reason, and body — in the space between the top safe margin
     * and that hint instead, on both PAL and NTSC. */
    if (help == SETUP_HELP_CONNECTION) {
        int content_h = (img_h_box ? img_h_box + img_gap : 0)
                       + (tch + lsp) + (sch + lsp * 2) + 2 * (sch + lsp);
        int avail = (fb->height - 8 - SAFE_Y_BOT) - SAFE_Y;
        if (avail > content_h) cur_y += (avail - content_h) / 2;
    }

    if (img_h_box > 0) {
        blit_fit_centered(fb, img_px, img_w, img_h,
                           fb->width / 2, cur_y + img_h_box / 2, img_w_box, img_h_box, 255);
        cur_y += img_h_box + img_gap;
    }

    draw_text(fb, (fb->width - text_width(fb, title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp;
    draw_text(fb, (fb->width - text_width(fb, reason, s1)) / 2, cur_y, reason, s1, COL_ERR);
    cur_y += sch + lsp * 2;

    if (help == SETUP_HELP_CONNECTION) {
        const char *l1 = "Check the server URL in jellyfin.conf:";
        draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_HINT);
        cur_y += sch + lsp;
        draw_text(fb, (fb->width - text_width(fb, g_cfg.server, s1)) / 2, cur_y, g_cfg.server, s1, COL_ITEM);
        cur_y += sch + lsp;
    } else {
        const char *l1 = "Create /media/fat/misterfin/jellyfin.conf with:";
        draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_HINT);
        cur_y += sch + lsp;
        static const char *fields[] = { "server_url", "api_key", "username" };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            draw_text(fb, (fb->width - text_width(fb, fields[i], s1)) / 2, cur_y, fields[i], s1, COL_ITEM);
            cur_y += sch + lsp;
        }
    }

    const char *hint = "A: exit";
    draw_text(fb, (fb->width - text_width(fb, hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* ── Quick Connect ────────────────────────────────────────────────────────── */

typedef enum {
    QC_STARTING,     /* asking the server to open a request */
    QC_WAITING,      /* code on screen, waiting for the user to approve it */
    QC_AUTHENTICATED,
    QC_UNAVAILABLE,  /* server has Quick Connect switched off */
    QC_FAILED        /* request expired, denied, or the server went away */
} QcState;

static pthread_mutex_t g_qc_mutex = PTHREAD_MUTEX_INITIALIZER;
static QcState  g_qc_state = QC_STARTING;
static char     g_qc_code[16] = "";

static QcState qc_get_state(char *code_out, int code_len)
{
    pthread_mutex_lock(&g_qc_mutex);
    QcState s = g_qc_state;
    if (code_out) {
        strncpy(code_out, g_qc_code, (size_t)code_len - 1);
        code_out[code_len - 1] = '\0';
    }
    pthread_mutex_unlock(&g_qc_mutex);
    return s;
}

static void qc_set_state(QcState s)
{
    pthread_mutex_lock(&g_qc_mutex);
    g_qc_state = s;
    pthread_mutex_unlock(&g_qc_mutex);
}

/* Runs the whole handshake off the main thread: the polling below waits on a
 * person walking to another device, which can be minutes. Blocking the main
 * loop for that would freeze the starfield and stop the screen responding to
 * a cancel press. */
#define QC_POLL_INTERVAL_SEC 3
#define QC_TIMEOUT_SEC       300

static void *quick_connect_thread(void *arg)
{
    (void)arg;

    int qc_enabled = jf_quick_connect_enabled(&g_cfg);
    if (qc_enabled == 0)  { qc_set_state(QC_UNAVAILABLE); return NULL; }
    if (qc_enabled  < 0)  { qc_set_state(QC_FAILED);       return NULL; }

    JfQuickConnect qc;
    if (!jf_quick_connect_initiate(&g_cfg, &qc)) { qc_set_state(QC_FAILED); return NULL; }

    pthread_mutex_lock(&g_qc_mutex);
    strncpy(g_qc_code, qc.code, sizeof(g_qc_code) - 1);
    g_qc_state = QC_WAITING;
    pthread_mutex_unlock(&g_qc_mutex);

    int elapsed = 0, consecutive_errors = 0, approved = 0;
    while (g_running) {
        /* Slept in short slices rather than one long sleep so quitting is
         * responsive: the app can exit while this is waiting, and a 3-second
         * sleep meant up to 3 seconds of a detached thread still running
         * against a torn-down process. */
        for (int slept = 0; slept < QC_POLL_INTERVAL_SEC * 10 && g_running; slept++)
            usleep(100 * 1000);
        if (!g_running) return NULL;
        elapsed += QC_POLL_INTERVAL_SEC;

        int poll = jf_quick_connect_poll(&g_cfg, &qc);
        if (poll == 1) { approved = 1; break; }
        if (poll < 0) {
            /* A poll failure is ambiguous — the request really being gone
             * looks the same as the wifi hiccupping. Tolerate a couple before
             * concluding the request is dead, so a blip doesn't throw away a
             * code the user is halfway through typing. */
            if (++consecutive_errors >= 3) { qc_set_state(QC_FAILED); return NULL; }
        } else {
            consecutive_errors = 0;
        }

        /* Checked AFTER the poll, not before it. Testing first meant the
         * final poll never ran, so an approval given in the last few seconds
         * was discarded — and it's single-use, so the user's code was burned
         * for nothing. */
        if (elapsed >= QC_TIMEOUT_SEC) break;
    }
    if (!approved) { qc_set_state(QC_FAILED); return NULL; }
    if (!g_running) return NULL;

    if (!jf_quick_connect_authenticate(&g_cfg, &qc)) { qc_set_state(QC_FAILED); return NULL; }

    jf_token_save(&g_cfg);   /* so this is a one-time step, not a per-launch one */
    qc_set_state(QC_AUTHENTICATED);
    return NULL;
}

static void draw_quick_connect(FBDev *fb)
{
    fb_clear(fb);
    draw_starfield(fb);

    char code[16];
    QcState state = qc_get_state(code, sizeof(code));

    const int ts = 2, s1 = 1;
    const int tch = 8 * ts, sch = 8 * s1, lsp = 6;

    /* Body height varies a lot by state — a one-line "Contacting server..."
     * versus the multi-line code-entry prompt — so it's measured per state
     * (mirroring the exact spacing used when actually drawing it below) and
     * the whole block centered around the screen's true middle. A single
     * fixed offset tuned only for the tallest case (QC_WAITING) left every
     * shorter state sitting visibly high, with a big gap before the bottom
     * "A: exit" hint. */
    int body_h;
    switch (state) {
    case QC_STARTING:
    case QC_AUTHENTICATED:
        body_h = sch;
        break;
    case QC_WAITING:
        body_h = (sch + lsp) + (sch + lsp * 3) + (8 * 3 + lsp * 3) + sch;
        break;
    case QC_UNAVAILABLE:
        body_h = (sch + lsp * 2) + (sch + lsp) + sch;
        break;
    case QC_FAILED:
    default:
        body_h = (sch + lsp * 2) + sch;
        if (server_https_needs_insecure_hint()) body_h += sch + lsp * 2;
        break;
    }
    int cur_y = fb->height / 2 - ((tch + lsp * 2) + body_h) / 2;

    static const char title[] = "Quick Connect";
    draw_text(fb, (fb->width - text_width(fb, title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp * 2;

    switch (state) {
    case QC_STARTING: {
        const char *msg = "Contacting server...";
        draw_text(fb, (fb->width - text_width(fb, msg, s1)) / 2, cur_y, msg, s1, COL_HINT);
        break;
    }
    case QC_WAITING: {
        const char *l1 = "In Jellyfin, open your user menu and";
        const char *l2 = "choose Quick Connect, then enter:";
        draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_HINT);
        cur_y += sch + lsp;
        draw_text(fb, (fb->width - text_width(fb, l2, s1)) / 2, cur_y, l2, s1, COL_HINT);
        cur_y += sch + lsp * 3;

        /* The code is the one thing being read off a CRT from across a room,
         * so it gets the largest text on screen by some margin. */
        draw_text(fb, (fb->width - text_width(fb, code, 3)) / 2, cur_y, code, 3, COL_SEL_FG);
        cur_y += 8 * 3 + lsp * 3;

        const char *l3 = "Waiting for approval...";
        draw_text(fb, (fb->width - text_width(fb, l3, s1)) / 2, cur_y, l3, s1, COL_ITEM);
        break;
    }
    case QC_AUTHENTICATED: {
        const char *msg = "Signed in. Starting...";
        draw_text(fb, (fb->width - text_width(fb, msg, s1)) / 2, cur_y, msg, s1, COL_WATCHED);
        break;
    }
    case QC_UNAVAILABLE: {
        const char *l1 = "Quick Connect is disabled on this server.";
        const char *l2 = "Enable it in Dashboard > General, or put an";
        const char *l3 = "API key on line 2 of jellyfin.conf.";
        draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_ERR);
        cur_y += sch + lsp * 2;
        draw_text(fb, (fb->width - text_width(fb, l2, s1)) / 2, cur_y, l2, s1, COL_HINT);
        cur_y += sch + lsp;
        draw_text(fb, (fb->width - text_width(fb, l3, s1)) / 2, cur_y, l3, s1, COL_HINT);
        break;
    }
    case QC_FAILED: {
        const char *l1 = "Quick Connect failed or timed out.";
        const char *l2 = "B: try again";
        draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_ERR);
        cur_y += sch + lsp * 2;
        /* An https server that can't be reached with verification on is
         * most likely a self-signed cert — surface the fix here too, since
         * a saved-token or QC user never touches the setup screen above. */
        if (server_https_needs_insecure_hint()) {
            const char *lt = "If HTTPS self-signed, set INSECURE_TLS in jellyfin.conf";
            draw_text(fb, (fb->width - text_width(fb, lt, s1)) / 2, cur_y, lt, s1, COL_HINT);
            cur_y += sch + lsp * 2;
        }
        draw_text(fb, (fb->width - text_width(fb, l2, s1)) / 2, cur_y, l2, s1, COL_ITEM);
        break;
    }
    }

    const char *hint = "A: exit";
    draw_text(fb, (fb->width - text_width(fb, hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* NOT a literal aspect-ratio box — blit_fit_centered's 5/3 pixel-aspect
 * correction means a typical portrait poster actually wants a box WIDER
 * than naive square-pixel math would suggest to look right on the real
 * screen (mirrors a proven pdh=160/max_pw=200 proportion, scaled down for
 * this smaller corner panel). */
#define BROWSE_COVER_W 175
#define BROWSE_COVER_H 140
/* Source width asked of the server for that panel. Sized to the panel and no
 * larger on purpose: fb_blit samples nearest-neighbour (no averaging), so a
 * source well above its destination does not soften the downscale, it just
 * drops different pixels — supersampling only pays once there is a filter to
 * pay it. Kept a touch over BROWSE_COVER_W because the box bounds both axes
 * and blit_fit_centered may fit to the height instead, leaving the drawn
 * width short of the full 175. */
#define BROWSE_COVER_DL_W 180

static uint8_t *g_browse_cover_px = NULL;
static int      g_browse_cover_w = 0, g_browse_cover_h = 0;
static char     g_browse_cover_item_id[JF_ID_LEN] = "";

/* Cache-file path for one item's cover. Keyed by item id plus
 * a few chars of the image tag, so replacing the artwork server-side lands on
 * a new filename rather than serving stale art — and by the width it was
 * fetched at, so changing BROWSE_COVER_DL_W re-fetches instead of serving the
 * old size forever. Without that last part every card already in the wild
 * keeps whatever resolution it first cached, which is a change that silently
 * does nothing for existing users and works only on a fresh install. */
static int cover_cache_path(const char *item_id, const char *tag, char *out, size_t outsz)
{
    char safe[JF_ID_LEN + 12];
    size_t j = 0;
    for (size_t i = 0; item_id[i] && j < JF_ID_LEN; i++) {
        char c = item_id[i];
        safe[j++] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
    }
    if (j < sizeof(safe) - 1) safe[j++] = '_';
    for (size_t i = 0; tag[i] && i < 8 && j < sizeof(safe) - 1; i++) {
        char c = tag[i];
        safe[j++] = isalnum((unsigned char)c) ? c : '_';
    }
    safe[j] = '\0';
    char dir[256];
    if (!cache_dir_path("covercache", dir, sizeof(dir))) return 0;
    int n = snprintf(out, outsz, "%s/%s_w%d.img", dir, safe, BROWSE_COVER_DL_W);
    return n >= 0 && (size_t)n < outsz;
}

/* Downloads one cover into the persistent cache if not already there.
 * Downloads to a ".tmp" sibling IN THE CACHE DIR, then renames it so the main
 * thread never reads a half-written file. Keeping both paths on one filesystem
 * also avoids the EXDEV failure that a /tmp-to-/media/fat rename would cause. */
static void cover_fetch_to_cache(const char *image_item_id, const char *tag,
                                  const char *item_id)
{
    if (!tag[0]) return;
    char cpath[384];
    if (!cover_cache_path(item_id, tag, cpath, sizeof(cpath))) return;
    struct stat st;
    if (stat(cpath, &st) == 0 && st.st_size > 0) return;   /* already cached */
    char dir[256];
    if (!cache_dir_ensure("covercache", dir, sizeof(dir))) return;
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
    if (jf_download_item_image(&g_cfg, image_item_id, "Primary", tag,
                                BROWSE_COVER_DL_W, tmp)) {
        if (rename(tmp, cpath) != 0) unlink(tmp);
    } else {
        unlink(tmp);
    }
}

/* The item id whose cover the current selection wants, or "" if none. */
static const char *browse_cover_wanted_id(void)
{
    JfItem *it = (g_item_count > 0) ? &g_items[g_sel] : NULL;
    int wants = it && it->image_tag[0] &&
        (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_MUSIC_VIDEO ||
         it->type == JF_TYPE_EPISODE ||
         it->type == JF_TYPE_SERIES || it->type == JF_TYPE_SEASON ||
         it->type == JF_TYPE_ARTIST || it->type == JF_TYPE_ALBUM || it->type == JF_TYPE_TRACK);
    return wants ? it->id : "";
}

/* Loads the selected row's cover into the top-right panel — from the
 * persistent cache ONLY, never the network. A cached cover shows instantly
 * with no freeze; an uncached one leaves the panel blank until the background
 * prefetch (cover_prefetch_thread) writes it to storage, at which point the
 * next browse redraw (~100ms, for the live clock/marquee) picks it up. So the
 * main thread never blocks on a per-item download — that was the scroll
 * freeze — and any list visited before shows every cover instantly. Stale art
 * is dropped up front so an item with no cover never inherits the previous
 * row's. */
static void browse_cover_load(void)
{
    JfItem *it = (g_item_count > 0) ? &g_items[g_sel] : NULL;
    const char *want = browse_cover_wanted_id();

    if (g_browse_cover_px && strcmp(g_browse_cover_item_id, want) == 0) return;

    if (g_browse_cover_px) { stbi_image_free(g_browse_cover_px); g_browse_cover_px = NULL; }
    g_browse_cover_w = g_browse_cover_h = 0;
    g_browse_cover_item_id[0] = '\0';
    if (want[0] == '\0') return;   /* this item has no cover */

    char cpath[384];
    if (!cover_cache_path(it->id, it->image_tag, cpath, sizeof(cpath))) return;
    int w = 0, h = 0;
    uint8_t *px = load_image_keep(cpath, &w, &h);
    if (!px) return;   /* not cached yet — blank; prefetch fills it, next redraw loads it */
    g_browse_cover_px = px;
    g_browse_cover_w = w; g_browse_cover_h = h;
    strncpy(g_browse_cover_item_id, it->id, sizeof(g_browse_cover_item_id) - 1);
    g_browse_cover_item_id[sizeof(g_browse_cover_item_id) - 1] = '\0';
}

/* Reclaims cover/grid cache entries written under an older filename key —
 * raising a fetch width leaves the previous files unreachable and stranded on
 * the card (see cache_sweep_superseded, which explains why this needs no
 * marker file and cannot touch the live cache).
 *
 * Off the main thread and detached: unlink is slow here (exFAT mounted sync)
 * and this must not delay the first frame. Nothing waits on the result, and
 * being killed mid-sweep at exit just leaves the remainder for the next
 * launch. */
static void *cache_sweep_thread(void *arg)
{
    (void)arg;
    char cover_dir[256], grid_dir[256];
    int n = 0;
    if (cache_dir_path("covercache", cover_dir, sizeof(cover_dir)))
        n += cache_sweep_superseded(cover_dir, ".img", "_w");
    if (cache_dir_path("gridcache", grid_dir, sizeof(grid_dir)))
        n += cache_sweep_superseded(grid_dir, ".dat", "_w");
    /* Only when it did something — on every launch after the first there is
     * nothing to find, and a line saying so each time is noise. */
    if (n > 0) jf_log_line("cache: reclaimed %d entries from a superseded key", n);
    return NULL;
}

static void start_cache_sweep(void)
{
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, cache_sweep_thread, NULL);   /* failure is a no-op */
    pthread_attr_destroy(&at);
}

/* ── browse hero backdrop ─────────────────────────────────────────────────
 * The selected row's wide artwork, behind the list, in the same rect the info
 * screen leads with — so drilling into a title continues a picture already on
 * screen instead of replacing one. Deliberately not the poster: that is
 * already in the top-right panel, and a 2:3 poster stretched across this rect
 * looks like exactly what it is.
 *
 * Held well under full strength beneath the same top-down wash the info
 * screen uses, because unlike that screen this one has a list of text over it
 * and legibility wins. */
/* 110, matching GRID_ALPHA: the mosaic background settled on that figure
 * after 40 and then 65 were both reported too faint, and this is the same
 * judgement about the same kind of picture behind the same UI. 77 was tried
 * first here and raised on that same feedback. The headroom is there for it —
 * list rows draw in COL_ITEM (0xCC), and a real backdrop averages well below
 * white, so even the un-washed top of the rect stays clear of the text. */
#define HERO_ALPHA  110           /* out of 255, before the wash */
/* 640, not fb->width: this is the widest the rect is ever drawn (the narrow
 * HDMI box is 480), one fetch width keeps one cache key, and the prefetch
 * thread has no FBDev to ask anyway. Same figure info_assets_load already
 * uses for the info screen's own backdrop. */
#define HERO_DL_W   640

/* (3*height)/4 is the closed-form pixel height for a 16:9-DAR image spanning
 * the full width on this platform's non-square pixels — the derivation
 * draw_info documents for its own backdrop. A Jellyfin backdrop is 16:9, so
 * it lands in this rect undistorted and needs no cropping. */
static int hero_rect_h(FBDev *fb) { return (3 * fb->height) / 4; }

static uint8_t *g_hero_px = NULL;     /* pre-composed, destination-sized, malloc'd */
static int      g_hero_w = 0, g_hero_h = 0;
static char     g_hero_item_id[JF_ID_LEN] = "";

/* 'h' prefix so a hero and a cover for the same item can't collide in the one
 * cache directory, plus the fetch width for the same reason cover_cache_path
 * carries it. */
static int hero_cache_path(const char *item_id, const char *tag, char *out, size_t outsz)
{
    char safe[JF_ID_LEN + 12];
    size_t j = 0;
    for (size_t i = 0; item_id[i] && j < JF_ID_LEN; i++) {
        char c = item_id[i];
        safe[j++] = isalnum((unsigned char)c) ? c : '_';
    }
    for (size_t i = 0; tag[i] && i < 6 && j < sizeof(safe) - 1; i++) {
        char c = tag[i];
        safe[j++] = isalnum((unsigned char)c) ? c : '_';
    }
    safe[j] = '\0';
    char dir[256];
    if (!cache_dir_path("covercache", dir, sizeof(dir))) return 0;
    int n = snprintf(out, outsz, "%s/h%s_w%d.img", dir, safe, HERO_DL_W);
    return n >= 0 && (size_t)n < outsz;
}

/* Mirror of cover_fetch_to_cache — same tmp-then-rename inside the cache dir
 * so the main thread never reads a half-written file. */
static void hero_fetch_to_cache(const char *img_id, const char *tag, const char *item_id)
{
    if (!tag[0] || !img_id[0]) return;
    char cpath[384];
    if (!hero_cache_path(item_id, tag, cpath, sizeof(cpath))) return;
    if (access(cpath, F_OK) == 0) return;

    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
    char dir[256];
    if (!cache_dir_ensure("covercache", dir, sizeof(dir))) return;
    if (jf_download_item_image(&g_cfg, img_id, "Backdrop/0", tag, HERO_DL_W, tmp)) {
        if (rename(tmp, cpath) != 0) unlink(tmp);
    } else {
        unlink(tmp);
    }
}

/* The item id whose backdrop the current selection wants, or "" if none.
 * Gated only on the tag being there: parse_item_fields already falls back to
 * the parent's backdrop, which is what gives every episode of a series the
 * series' artwork. */
static const char *browse_hero_wanted_id(void)
{
    JfItem *it = (g_item_count > 0) ? &g_items[g_sel] : NULL;
    return (it && it->backdrop_tag[0]) ? it->id : "";
}

/* Same persistent-cache-only rule as browse_cover_load: never a network fetch
 * on the main thread, so scrolling can't freeze. An uncached backdrop leaves
 * the background black until the prefetch writes it and the next redraw picks
 * it up. */
static void browse_hero_load(FBDev *fb)
{
    JfItem *it = (g_item_count > 0) ? &g_items[g_sel] : NULL;
    const char *want = browse_hero_wanted_id();
    int dh = hero_rect_h(fb);

    if (g_hero_px && g_hero_h == dh && strcmp(g_hero_item_id, want) == 0) return;

    if (g_hero_px) { free(g_hero_px); g_hero_px = NULL; }
    g_hero_w = g_hero_h = 0;
    g_hero_item_id[0] = '\0';
    if (want[0] == '\0') return;

    char cpath[384];
    if (!hero_cache_path(it->id, it->backdrop_tag, cpath, sizeof(cpath))) return;
    int w = 0, h = 0;
    uint8_t *px = load_image_keep(cpath, &w, &h);
    if (!px) return;
    g_hero_px = compose_backdrop_wash(px, w, h, fb->width, dh, HERO_ALPHA);
    stbi_image_free(px);
    if (!g_hero_px) return;
    g_hero_w = fb->width; g_hero_h = dh;
    strncpy(g_hero_item_id, it->id, sizeof(g_hero_item_id) - 1);
    g_hero_item_id[sizeof(g_hero_item_id) - 1] = '\0';
}

/* Background fill of the persistent cover cache for the current list, so
 * scrolling reads covers without a network request. Snapshots the item
 * ids/tags (never touches g_items off-thread) and stops early if the list
 * changed under it (g_cover_gen). */
typedef struct {
    int gen, n;
    struct { char id[JF_ID_LEN], img_id[JF_ID_LEN], tag[JF_ID_LEN],
                  bd_id[JF_ID_LEN], bd_tag[JF_ID_LEN]; } items[JF_PAGE_SIZE];
} CoverPrefetch;

static void *cover_prefetch_thread(void *arg)
{
    CoverPrefetch *cp = (CoverPrefetch *)arg;
    for (int i = 0; i < cp->n; i++) {
        if (g_cover_gen != cp->gen) break;   /* navigated away — stop */
        cover_fetch_to_cache(cp->items[i].img_id, cp->items[i].tag, cp->items[i].id);
        if (g_cover_gen != cp->gen) break;
        /* After the cover, not before: the poster is the thing the user is
         * looking straight at, the backdrop only fills in behind it. */
        hero_fetch_to_cache(cp->items[i].bd_id, cp->items[i].bd_tag, cp->items[i].id);
    }
    free(cp);
    return NULL;
}

static void start_cover_prefetch(void)
{
    CoverPrefetch *cp = malloc(sizeof(*cp));
    if (!cp) return;
    cp->gen = g_cover_gen;
    cp->n = 0;
    for (int i = 0; i < g_item_count && cp->n < JF_PAGE_SIZE; i++) {
        JfItem *it = &g_items[i];
        int wants = it->image_tag[0] &&
            (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_MUSIC_VIDEO ||
             it->type == JF_TYPE_EPISODE ||
             it->type == JF_TYPE_SERIES || it->type == JF_TYPE_SEASON ||
             it->type == JF_TYPE_ARTIST || it->type == JF_TYPE_ALBUM || it->type == JF_TYPE_TRACK);
        if (!wants) continue;
        snprintf(cp->items[cp->n].id,     JF_ID_LEN, "%s", it->id);
        snprintf(cp->items[cp->n].img_id, JF_ID_LEN, "%s", it->image_item_id);
        snprintf(cp->items[cp->n].tag,    JF_ID_LEN, "%s", it->image_tag);
        snprintf(cp->items[cp->n].bd_id,   JF_ID_LEN, "%s", it->backdrop_item_id);
        snprintf(cp->items[cp->n].bd_tag,  JF_ID_LEN, "%s", it->backdrop_tag);
        cp->n++;
    }
    if (cp->n == 0) { free(cp); return; }
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, cover_prefetch_thread, cp) != 0) free(cp);
    pthread_attr_destroy(&at);
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
static int draw_clock_color(FBDev *fb, uint8_t r, uint8_t g, uint8_t b)
{
    time_t now_t = time(NULL);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);
    char clock_buf[8];
    snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", now_tm.tm_hour, now_tm.tm_min);
    int clock_right = fb->width - SPINNER_SIZE - SPINNER_MARGIN - 8;
    int clock_x = clock_right - text_width(fb, clock_buf, 1);
    draw_text(fb, clock_x, SAFE_Y + 4, clock_buf, 1, r, g, b);
    return clock_x;
}

static int draw_clock(FBDev *fb)
{
    return draw_clock_color(fb, COL_HINT);
}

/* Small filled-diamond "rating star" icon — this build's bitmap font is
 * ASCII-only (no ★ glyph), so a rating number is preceded by this instead
 * of the word "Rating". 5x5 at scale 1. */
static void draw_star_icon(FBDev *fb, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    static const uint8_t rows[5] = { 0x04, 0x0E, 0x1F, 0x0E, 0x04 };
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            if ((rows[row] >> (4 - col)) & 1)
                fb_fill_rect_alpha(fb, x + col * scale, y + row * scale, scale, scale, r, g, b, 255);
        }
    }
}

static void draw_top_bar(FBDev *fb, const char *title)
{
    int clock_x = draw_clock(fb);

    int title_x0 = SAFE_X;
    int title_x1 = clock_x - 12;
    int title_w  = text_width(fb, title, 2);
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
    const char *msg = "Exit? [B: yes  A: no]";
    int scale = 2;
    int tw = text_width(fb, msg, scale);
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
/* Max text width before truncating (at scale 2, 8px/char*scale — 160 = 10
 * characters, so anything longer than 10 chars truncates to 7 + "..."). No
 * longer a collision concern the way it was with the old fixed
 * CAROUSEL_SPACING layout (raising this used to shrink the gap between
 * neighbors, down to zero in the worst case — confirmed on hardware once as
 * "Continue"/"Documentaries" running into each other with no gap at all):
 * card spacing is now computed dynamically from each card's actual measured
 * width (see carousel_card_width/draw_carousel_cards) plus a fixed
 * CAROUSEL_GAP, so this only controls how long a name can get before it
 * truncates — not how close cards sit to each other. */
#define CAROUSEL_CARD_W 160

/* The count line's wording — shared by draw_library_card (drawing it) and
 * carousel_card_width (measuring it, for spacing) so the two can't drift
 * apart into mismatched text. */
static void carousel_format_count(const JfItem *it, int64_t count, char *buf, size_t bufsz)
{
    const char *ct = it->collection_type;
    if (view_is_synthetic(it))
        snprintf(buf, bufsz, "%lld item%s", (long long)count, count == 1 ? "" : "s");
    else if (!strcmp(ct, "movies"))
        snprintf(buf, bufsz, "%lld movie%s", (long long)count, count == 1 ? "" : "s");
    else if (!strcmp(ct, "tvshows"))
        snprintf(buf, bufsz, "%lld series", (long long)count);
    else if (!strcmp(ct, "music"))
        snprintf(buf, bufsz, "%lld album%s", (long long)count, count == 1 ? "" : "s");
    else if (!strcmp(ct, "musicvideos"))
        snprintf(buf, bufsz, "%lld video%s", (long long)count, count == 1 ? "" : "s");
    else
        snprintf(buf, bufsz, "%lld item%s", (long long)count, count == 1 ? "" : "s");
}

static void draw_library_card(FBDev *fb, const JfItem *it, int64_t count, int cx, int cy, int active)
{
    char name[64];
    strncpy(name, it->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    truncate_to_width(fb, name, 2, CAROUSEL_CARD_W);
    /* Can't ternary a multi-arg color macro straight into a call — the
     * comma in e.g. COL_TITLE splits into extra call arguments instead of
     * picking r/g/b together, silently mis-coloring this (caught by eye in
     * the --preview-browse render, not by the compiler). */
    if (active) draw_text(fb, cx - text_width(fb, name, 2) / 2, cy - 10, name, 2, COL_TITLE);
    else        draw_text(fb, cx - text_width(fb, name, 2) / 2, cy - 10, name, 2, 0xFF, 0xFF, 0xFF);

    if (count >= 0) {
        char cbuf[32];
        carousel_format_count(it, count, cbuf, sizeof(cbuf));
        draw_text(fb, cx - text_width(fb, cbuf, 1) / 2, cy + 12, cbuf, 1, COL_HINT);
    }
}

/* This card's on-screen footprint (the wider of its two centered lines,
 * name at scale 2 and the count at scale 1) — used to space cards so the
 * GAP between any two neighbors' text is always the same regardless of how
 * long either name is, instead of the old fixed CAROUSEL_SPACING per card
 * (which left short names like "Movies"/"Music" with a visibly wider gap
 * than a long name truncated all the way to CAROUSEL_CARD_W). */
static double carousel_card_width(FBDev *fb, int i)
{
    char name[64];
    strncpy(name, g_items[i].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    truncate_to_width(fb, name, 2, CAROUSEL_CARD_W);
    double w = text_width(fb, name, 2);

    if (g_view_counts[i] >= 0) {
        char cbuf[32];
        carousel_format_count(&g_items[i], g_view_counts[i], cbuf, sizeof(cbuf));
        double cw = text_width(fb, cbuf, 1);
        if (cw > w) w = cw;
    }
    return w;
}

/* Root screen (g_stack_depth == 1, FRAME_VIEWS) — a horizontal carousel of
 * library cards instead of the regular list, since a handful of libraries
 * (movies/TV/music, ...) reads better as a few big blocks than as rows.
 * Every library gets a slot along the strip (see draw_carousel_cards);
 * ones that don't fit on screen simply run off the edge and get clipped. */

/* Fixed edge-to-edge gap between adjacent cards' text (see
 * carousel_card_width) — the value that stays constant regardless of name
 * length, replacing the old fixed CAROUSEL_SPACING-per-card approach. */
#define CAROUSEL_GAP 74
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
 * gets the active (yellow) treatment.
 *
 * Card centers are laid out cumulatively from each card's own measured
 * width (abscenter[]) rather than at fixed multiples of a spacing constant,
 * so center[i+1] - center[i] = w[i]/2 + CAROUSEL_GAP + w[i+1]/2 always —
 * the visible gap is the same CAROUSEL_GAP no matter how long either
 * neighbor's name is. visual_sel (possibly fractional, mid-slide) maps to
 * a reference point interpolated the same way between the two abscenter[]
 * entries it sits between, so the screen-centered card is always exactly
 * the one at visual_sel. */
static void draw_carousel_cards(FBDev *fb, double visual_sel, int cy)
{
    int center_cx = fb->width / 2;
    int n = g_item_count;
    if (n <= 0) return;

    double half_w[JF_MAX_ITEMS];
    double abscenter[JF_MAX_ITEMS];
    for (int i = 0; i < n; i++) half_w[i] = carousel_card_width(fb, i) / 2.0;
    abscenter[0] = 0.0;
    for (int i = 1; i < n; i++)
        abscenter[i] = abscenter[i - 1] + half_w[i - 1] + CAROUSEL_GAP + half_w[i];

    int old_i = (int)floor(visual_sel);
    if (old_i < 0) old_i = 0;
    if (old_i > n - 1) old_i = n - 1;
    double ref;
    if (old_i + 1 < n) {
        double frac = visual_sel - old_i;
        ref = abscenter[old_i] + (abscenter[old_i + 1] - abscenter[old_i]) * frac;
    } else {
        ref = abscenter[old_i];
    }

    int active_i = (int)(visual_sel + (visual_sel >= 0 ? 0.5 : -0.5));
    for (int i = 0; i < n; i++) {
        int cx = center_cx + (int)lround(abscenter[i] - ref);
        if (cx < -CAROUSEL_CARD_W || cx > fb->width + CAROUSEL_CARD_W) continue;
        if (i == active_i) continue;   /* drawn last, on top, in case of any overlap at tight spacing */
        draw_library_card(fb, &g_items[i], g_view_counts[i], cx, cy, 0);
    }
    if (active_i >= 0 && active_i < n) {
        int cx = center_cx + (int)lround(abscenter[active_i] - ref);
        draw_library_card(fb, &g_items[active_i], g_view_counts[active_i], cx, cy, 1);
    }
}

static void draw_carousel_hint(FBDev *fb)
{
    const char *hint = "LEFT/RIGHT: browse   B:select   SELECT:list view   A:exit";
    draw_text(fb, (fb->width - text_width(fb, hint,1))/2,
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

/* One quiet line under the title when a newer release exists — home
 * carousel only, the list view stays clean. START already opens the About
 * screen from anywhere, and that's where the install button lives, so
 * this is a signpost rather than a new path. Hidden while a download is
 * running or applied: About is telling that story to whoever started it.
 * Drawn by BOTH the settled carousel frame and every slide-animation
 * frame — left out of the animation it blinked off during each
 * LEFT/RIGHT transition while the title/clock/hints stayed put. */
static void draw_carousel_update_notice(FBDev *fb)
{
    UpdateState  us;
    InstallState is;
    update_get_state(&us, &is, NULL, 0);
    if (us == UPD_AVAILABLE && is == INST_IDLE) {
        const char *upd = "START:update available";
        draw_text(fb, SAFE_X, SAFE_Y + 20, upd, 1, COL_HINT);
    }
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
    double slide_t0 = now_sec();
    for (int s = 1; s <= CAROUSEL_SLIDE_STEPS; s++) {
        double t = (double)s / CAROUSEL_SLIDE_STEPS;
        double visual = old_sel + (new_sel - old_sel) * carousel_ease(t);

        /* The background used to cross-fade through full black, switching
         * libraries at the midpoint where nothing was visible — which is what
         * the sideways push replaced. Leaving the fade in put a hard black
         * blink at the start of every push, one hiding the other. Switched at
         * the first step instead of the midpoint, so the push starts with the
         * slide rather than halfway through it. */
        if (!switched) {
            grid_covers_sync(fb, &g_items[new_sel]);
            switched = 1;
        }

        fb_clear(fb);
        draw_grid_background(fb);
        draw_grid_gradient(fb);
        draw_top_bar(fb, "MiSTerFin");
        draw_carousel_update_notice(fb);
        draw_carousel_cards(fb, visual, cy);
        draw_carousel_hint(fb);
        fb_flip(fb);

        /* Hold this step until its slot in the 60Hz timeline. When vsync
         * works, fb_flip already blocked past this and the wait is a no-op.
         * When it does not, this is what makes the slide a slide rather than
         * six frames drawn as fast as the mosaic happens to allow. */
        double step_target = slide_t0 + (double)s / 60.0;
        double step_left   = step_target - now_sec();
        if (step_left > 0.0) usleep((useconds_t)(step_left * 1000000.0));
    }
}

static void draw_browse_carousel(FBDev *fb)
{
    if (g_item_count == 0) {
        fb_clear(fb);
        draw_top_bar(fb, "MiSTerFin");
        const char *msg = "No libraries found";
        draw_text(fb, (fb->width - text_width(fb, msg, 1))/2, fb->height/2, msg, 1, COL_HINT);
        fb_flip(fb);
        return;
    }

    grid_covers_sync(fb, &g_items[g_sel]);

    fb_clear(fb);
    draw_grid_background(fb);
    draw_grid_gradient(fb);
    draw_top_bar(fb, "MiSTerFin");
    draw_carousel_update_notice(fb);
    draw_carousel_cards(fb, (double)g_sel, carousel_cy(fb));
    draw_carousel_hint(fb);

    fb_flip(fb);
}

static void draw_browse(FBDev *fb)
{
    BrowseFrame *f = &g_stack[g_stack_depth - 1];
    if (f->kind == FRAME_VIEWS && !g_root_list_mode) { draw_browse_carousel(fb); return; }

    browse_cover_load();
    browse_hero_load(fb);

    fb_clear(fb);

    /* One opaque copy — the dimming and the wash are already in its pixels
     * (see compose_backdrop_wash). Below the rect the frame stays as fb_clear
     * left it, which is exactly what the wash fades into. */
    if (g_hero_px)
        fb_blit_opaque(fb, g_hero_px, g_hero_w, g_hero_h, 0, 0, g_hero_w, g_hero_h);

    const char *title = f->title[0] ? f->title : "MiSTerFin";
    draw_top_bar(fb, title);

    if (g_item_count == 0) {
        const char *msg = "Nothing here";
        draw_text(fb, (fb->width - text_width(fb, msg, 1))/2, fb->height/2, msg, 1, COL_HINT);
        fb_flip(fb);
        return;
    }

    int cover_panel_x = fb->width - SAFE_X - BROWSE_COVER_W;
    /* -3 lines the panel's top up with the first row's selection highlight
     * top (that highlight starts at sel_y-3, see the glide block below) —
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
        int dw = (int)((double)g_browse_cover_w / g_browse_cover_h * dh * par_correction(fb) + 0.5);
        if (dw > BROWSE_COVER_W) dh = dh * BROWSE_COVER_W / dw;
        int cy = cover_panel_y + dh / 2;
        blit_fit_centered(fb, g_browse_cover_px, g_browse_cover_w, g_browse_cover_h,
                           cx, cy, BROWSE_COVER_W, BROWSE_COVER_H, 255);
    }

    int visible = visible_rows(fb);
    int end = g_scroll + visible;
    if (end > g_item_count) end = g_item_count;

    /* The highlight glides to the selected row instead of jumping to it, and
     * is drawn ONCE here rather than inside the row loop — mid-glide it sits
     * between two rows, so it no longer belongs to any single one of them.
     * Rows draw their text afterwards and therefore still land on top of it.
     *
     * Tracked in visible-row units (g_sel - g_scroll), which is what makes
     * scrolling behave: once the cursor reaches the bottom row the list
     * scrolls under it and that difference stops changing, so the bar
     * correctly stays put instead of trying to walk off the screen.
     *
     * dt is real elapsed time rather than a per-call constant because
     * draw_browse's own tick rate is not fixed (the grid mosaic background
     * costs several times what a plain list row does), and it is clamped at
     * both ends: the list is redrawn continuously while anything is moving,
     * but a screen sitting idle can go a long time between redraws and one
     * huge dt would make the next keypress snap instead of glide. */
    {
        double target = (double)(g_sel - g_scroll);
        if (g_sel_anim < 0.0 || fabs(target - g_sel_anim) > (double)visible) {
            /* First draw of this list, or a jump too far to read as motion
             * (a page-sized move — see browse_move_sel). */
            g_sel_anim = target;
        } else {
            static double prev_t = 0.0;
            double now = now_sec();
            double dt  = (prev_t > 0.0) ? now - prev_t : 0.0;
            prev_t = now;
            if (dt < 0.0)  dt = 0.0;      /* clock went backwards */
            if (dt > 0.05) dt = 0.05;     /* returning from an idle screen */
            double k = 1.0 - exp(-dt / 0.055);   /* ~95% of the way in 0.16s */
            g_sel_anim += (target - g_sel_anim) * k;
            /* Settle exactly, so an idle list stops redrawing a bar that is
             * one hundredth of a pixel from where it belongs. */
            if (fabs(target - g_sel_anim) < 0.01) g_sel_anim = target;
        }
        int sel_y = SAFE_Y + 24 + (int)(g_sel_anim * ROW_H + 0.5);
        fb_fill_rect_alpha(fb, SAFE_X - 4, sel_y - 3,
                           row_max_w + 8, ROW_H - 2, COL_SEL_BG, 220);
    }

    for (int i = g_scroll; i < end; i++) {
        int row = i - g_scroll;
        int y   = SAFE_Y + 24 + row * ROW_H;
        int is_sel = (i == g_sel);
        JfItem *it = &g_items[i];

        char line1[280];
        if (it->year[0] &&
            (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_MUSIC_VIDEO ||
             it->type == JF_TYPE_SERIES))
            snprintf(line1, sizeof(line1), "%s%s (%s)", type_folder_icon(it->type), it->name, it->year);
        else
            snprintf(line1, sizeof(line1), "%s%s", type_folder_icon(it->type), it->name);
        truncate_to_width(fb, line1, 1, row_max_w);

        /* Album: year + track count instead of a runtime — there's no
         * single "duration" for a whole album. Track: just its own
         * duration — no watched/resume state, which doesn't make sense for
         * an individual song the way it does for a video. Movie, music video,
         * and episode rows show runtime plus watched/resume state. */
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
            /* Both counts ride along on the listing request itself — season
             * count as ChildCount (confirmed a Series' own ChildCount means
             * exactly this, unlike a top-level library view's, see
             * jf_count_items's comment), episode count as RecursiveItemCount.
             * Neither costs an extra round-trip any more. */
            int ep = it->recursive_item_count;
            if (it->child_count > 0 && ep > 0)
                snprintf(line2, sizeof(line2), "%d season%s - %d episode%s",
                         it->child_count, it->child_count == 1 ? "" : "s",
                         ep, ep == 1 ? "" : "s");
            else if (it->child_count > 0)
                snprintf(line2, sizeof(line2), "%d season%s",
                         it->child_count, it->child_count == 1 ? "" : "s");
        } else if (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_MUSIC_VIDEO ||
                   it->type == JF_TYPE_EPISODE) {
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
        truncate_to_width(fb, line2, 1, row_max_w);

        /* The highlight itself is drawn above, before this loop — all that is
         * left per row is which colour its text takes. */
        if (is_sel) {
            draw_text(fb, SAFE_X, y, line1, 1, COL_SEL_FG);
            if (line2[0]) draw_text(fb, SAFE_X, y + 11, line2, 1, l2r, l2g, l2b);
        } else {
            draw_text(fb, SAFE_X, y, line1, 1, COL_ITEM);
            if (line2[0]) draw_text(fb, SAFE_X, y + 11, line2, 1, l2r, l2g, l2b);
        }
    }

    /* Absolute position in the whole list, not position within the loaded
     * window — the window is an implementation detail and showing "3/128"
     * partway through a 500-item library would be actively misleading. */
    int64_t total_rows = g_total_count > 0 ? g_total_count : g_item_count;
    if (total_rows > visible) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%lld",
                 g_window_start + g_sel + 1, (long long)total_rows);
        draw_text(fb, fb->width - text_width(fb, buf,1) - SAFE_X,
                  fb->height - 8 - SAFE_Y_BOT, buf, 1, COL_HINT);
    }

    int showing_artists = g_item_count > 0 && g_items[0].type == JF_TYPE_ARTIST;
    const char *hint =
        (g_stack_depth > 1 && showing_artists) ? "B:select  SELECT:shuffle library  A:back" :
        (g_stack_depth > 1)                    ? "B:select  A:back" :
                                                  "B:select  SELECT:cover view  A:exit";
    draw_text(fb, (fb->width - text_width(fb, hint,1))/2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

static void draw_info(FBDev *fb)
{
    fb_clear(fb);

    /* Cast row is disabled for now (see the commented-out block further
     * down) — too small to read well at this resolution, per user
     * feedback. cast_thumb is no longer subtracted from hero_h's budget,
     * so the banner/logo area and the text below both get more room than
     * before. overview_lines is still the one fixed-size element below the
     * banner (font row height doesn't shrink with resolution) — hero_h is
     * solved from whatever's left, capped at 150 (the original PAL-tuned
     * size) so this is a no-op at 288 active lines. */
    const int cast_thumb = 34;   /* kept as a constant for when the cast row above is re-enabled */
    (void)cast_thumb;            /* unused while that row is #if 0'd out below */
    const int overview_lines = 3;
    int hero_h = fb->height - 58 - overview_lines * 10;
    if (hero_h > 150) hero_h = 150;
    if (hero_h < 80) hero_h = 80;

    /* Backdrops are Jellyfin's standard 16:9 — shown here at that real
     * physical aspect ratio, using the WHOLE image (fb_blit scales/
     * compresses every source row into the target box, it doesn't crop),
     * spanning the full width, glued to the top edge. (3*fb->height)/4 is
     * the closed-form pixel height for a 16:9-DAR image spanning fb->width
     * on this platform's non-square pixels (same derivation as
     * par_correction(), simplifies cleanly for the 16:9 case specifically)
     * — the first attempt at this got the target height right but then
     * CROPPED down to it instead of scaling the full image into it, which
     * was the actual bug (discarding real picture content unnecessarily).
     * hero_h itself (logo/title/ty layout below) is unchanged — this only
     * affects how tall the image+gradient extends above/behind that
     * existing content. */
    int hero_h_full = (3 * fb->height) / 4;
    if (hero_h_full < hero_h) hero_h_full = hero_h;
    if (g_backdrop_px) {
        fb_blit(fb, g_backdrop_px, g_backdrop_w, g_backdrop_h, 0, 0, fb->width, hero_h_full, 255);
    } else {
        fb_fill_rect_alpha(fb, 0, 0, fb->width, hero_h_full, 0x18, 0x18, 0x18, 255);
    }

    /* Legibility gradient over the whole image: transparent at the very
     * top, fully opaque black by the image's own bottom edge so it blends
     * into the solid black area below (and behind whatever of the existing
     * title/logo/text happens to land on top of it, unchanged position). */
    for (int gy = 0; gy < hero_h_full; gy++) {
        int a = gy * 255 / (hero_h_full - 1);
        fb_fill_rect_alpha(fb, 0, gy, fb->width, 1, 0, 0, 0, (uint8_t)a);
    }

    draw_clock(fb);

    /* Logo (or the text fallback) is centered horizontally and anchored
     * toward the BOTTOM of the 0..hero_h band (not its vertical middle) —
     * per feedback, low in the banner reads better than dead-center.
     * Replicates blit_fit_centered's own dw/dh calc so the box's actual
     * (aspect-fit) size is known up front, same technique already used
     * for draw_browse's cover panel. */
    int logo_max_h = 44;
    int logo_cy = hero_h - logo_max_h / 2 + 3;
    if (g_logo_px) {
        int dh = logo_max_h;
        int dw = (int)((double)g_logo_w / g_logo_h * dh * par_correction(fb) + 0.5);
        if (dw > 480) { dh = dh * 480 / dw; dw = 480; }
        blit_fit_centered(fb, g_logo_px, g_logo_w, g_logo_h,
                           fb->width / 2, logo_cy, dw, dh, 255);
    } else {
        char title_line[300];
        if (g_info_item.year[0])
            snprintf(title_line, sizeof(title_line), "%s (%s)", g_info_item.name, g_info_item.year);
        else
            snprintf(title_line, sizeof(title_line), "%s", g_info_item.name);
        draw_text(fb, (fb->width - text_width(fb, title_line, 1)) / 2, logo_cy - 4,
                  title_line, 1, COL_SEL_FG);
    }

    /* Text block (year/rating/status + overview) is anchored from the
     * BOTTOM up, not stacked right under the banner: with the cast row
     * gone there's a lot of freed vertical space, and the goal is for the
     * block's bottom edge to land close to (not all the way down to)
     * where the old cast row used to sit, just above the hint bar —
     * rather than leaving that space empty right under the banner.
     * content_h_estimate mirrors the same "16 for the rating row + 10px
     * per overview line" budget hero_h's own formula above assumes. */
    int content_h_estimate = 16 + overview_lines * 10 + 4;
    int content_bottom = fb->height - 8 - SAFE_Y_BOT - 34;
    int ty = content_bottom - content_h_estimate;
    if (ty < hero_h + 4) ty = hero_h + 4;
    int tw = fb->width - 2 * SAFE_X;

    /* Year + rating (gold star icon, not the word "Rating" — this build's
     * font has no ★ glyph) on the left, runtime/watched-or-resume status
     * on the right — same row, opposite ends of the safe zone. */
    int lx = SAFE_X;
    int left_info_shown = 0;
    if (g_info_item.year[0]) {
        draw_text(fb, lx, ty, g_info_item.year, 1, COL_DIM);
        lx += text_width(fb, g_info_item.year, 1) + 8;
        left_info_shown = 1;
    }
    if (g_info_item.community_rating > 0) {
        draw_star_icon(fb, lx, ty + 1, 1, 0xFF, 0xD7, 0x00);
        lx += 5 + 4;
        char rating_buf[8];
        snprintf(rating_buf, sizeof(rating_buf), "%.1f", g_info_item.community_rating);
        draw_text(fb, lx, ty, rating_buf, 1, COL_DIM);
        left_info_shown = 1;
    }

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
        int sx = fb->width - SAFE_X - text_width(fb, status, 1);
        if (g_info_item.played)                          draw_text(fb, sx, ty, status, 1, COL_WATCHED);
        else if (g_info_item.resume_ticks > 0)            draw_text(fb, sx, ty, status, 1, COL_RESUME);
        else                                              draw_text(fb, sx, ty, status, 1, COL_DIM);
    }
    if (status[0] || left_info_shown) ty += 16;

    if (g_info_item.overview[0])
        ty += draw_wrapped(fb, SAFE_X, ty, g_info_item.overview, 1, tw, overview_lines, COL_ITEM);
    ty += 4;

#if 0
    /* Cast row — disabled for now, per user feedback: the headshots are too
     * small to read well at this resolution either way. Kept here (not
     * deleted) in case it's worth reviving — e.g. at a bigger thumb size,
     * or once there's more vertical room to work with. cast_thumb is still
     * defined above (just no longer subtracted from hero_h's budget) so
     * re-enabling this is a matter of restoring that subtraction too. */
    /* Cast row — small headshots only, no name labels: there isn't enough
     * vertical room left at this resolution for both a photo and legible
     * text per person. */
    int cast_n = g_info_item.cast_count;
    if (cast_n > CAST_DISPLAY_MAX) cast_n = CAST_DISPLAY_MAX;
    if (cast_n > 0) {
        int thumb = cast_thumb;
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
#endif

    const char *hint = (g_info_item.resume_ticks > 0 && !g_info_item.played)
        ? "B:resume  SELECT:restart  A:back"
        : "B:play  A:back";
    draw_text(fb, (fb->width - text_width(fb, hint,1))/2,
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
    draw_text(fb, (fb->width - text_width(fb, label, 1)) / 2, y + 10, label, 1, COL_ITEM);
}

static JfStreamProfile stream_profile(void);   /* forward decl — defined with the playback code */

static void draw_paused(FBDev *fb, const char *name, double pos)
{
    fb_sync_back(fb);

    int cy = fb->height / 2 - 16;
    fb_fill_rect_alpha(fb, 0, cy - 6, fb->width, 50, 0, 0, 0, 200);

    const char *ps = "|| PAUSED";
    draw_text(fb, (fb->width - text_width(fb, ps, 2)) / 2, cy, ps, 2, 0xFF, 0xFF, 0x00);

    char nbuf[64];
    snprintf(nbuf, sizeof(nbuf), "%.60s", name);
    draw_text(fb, (fb->width - text_width(fb, nbuf, 1)) / 2, cy + 20, nbuf, 1, COL_ITEM);

    /* The transcode actually in effect. Shown because it's configurable now
     * and the whole reason it is configurable is to be swept on hardware —
     * having to infer which profile is live from a config file you edited
     * three reboots ago is exactly how that measurement goes wrong. */
    {
        JfStreamProfile p = stream_profile();
        char pbuf[48];
        snprintf(pbuf, sizeof(pbuf), "%dx%d @ %.1f Mbps",
                 p.max_width, p.max_height, p.video_bitrate / 1000000.0);
        draw_text(fb, (fb->width - text_width(fb, pbuf, 1)) / 2, cy + 32, pbuf, 1, COL_HINT);
    }

    /* -20 leaves only ~2px between the timeline's time label and the hint
     * row below on PAL too (same formula), but it's only been reported as
     * too tight on NTSC — leaving PAL's spacing exactly as-is per instr. */
    draw_timeline(fb, fb->height - 8 - SAFE_Y_BOT - (fb->height == 288 ? 20 : 28), pos,
                  (double)g_info_item.runtime_ticks / 10000000.0);

    /* SELECT now opens a picker with an audio tab as well, so it's worth
     * offering on a title that has alternate audio but no subtitles at all —
     * the old condition hid it in exactly that case. */
    /* VSync toggle is a no-op in page-flip (true interlace) mode — that
     * path is always tear-free via hardware page-flip regardless of the
     * /tmp/misterdvd_vsync flag (see draw_slice()'s pf_active gate in
     * docker/vo_fbdev.c), so don't offer or hint at a control that does
     * nothing there. g_pageflip_mode, not fb->line_double: the hint must
     * track the same flag the INP_L/INP_R handling is gated on, and a
     * progressive HDMI canvas line-doubles WITHOUT page flipping. */
    const char *hint;
    if (g_pageflip_mode) {
        hint = (g_info_item.sub_count > 0 || g_info_item.audio_count > 1)
            ? "B:resume  A:stop  SELECT:tracks"
            : "B:resume  A:stop";
    } else {
        hint = (g_info_item.sub_count > 0 || g_info_item.audio_count > 1)
            ? "B:resume  A:stop  L/R:vsync  SELECT:tracks"
            : "B:resume  A:stop  L/R:vsync";
    }
    draw_text(fb, (fb->width - text_width(fb, hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(fb);
}

/* ── playback (mplayer slave-mode) ───────────────────────────────────────── */

static pid_t   g_player_pid      = -1;
/* Only set when the current stream is https:// — mplayer's bundled FFmpeg
 * has no TLS backend, so curl fetches it into a FIFO instead (see
 * stream_via_curl_if_https). -1 means the player is reading its URL
 * directly, the ordinary http:// path, unchanged. */
static pid_t   g_curl_pid        = -1;
static int     g_cmd_fd          = -1;
static double  g_play_offset     = 0.0;
static double  g_play_start_wall = 0.0;
static int     g_paused          = 0;
static double  g_pause_wall      = 0.0;
static char    g_play_session_id[64];
static double  g_last_progress_report = 0.0;

/* Playback progress is fire-and-forget (the result is never read), but the
 * curl round-trip is blocking — running it on the main thread froze the
 * now-playing animation for its duration every PROGRESS_REPORT_INTERVAL (and
 * stuttered menu redraws). It's invisible during video only because mplayer
 * owns the picture there. Fire it on a detached thread instead. The args are
 * COPIED into a heap struct: the main thread may start the next track
 * (regenerating g_play_session_id / overwriting g_items) before this thread
 * runs, so it must not read those globals. jellyfin.c's request path forks
 * its own curl with stack-local buffers, so a concurrent caller is safe —
 * same as the grid-prefetch thread. */
typedef struct { char item_id[JF_ID_LEN]; char session[64]; int64_t pos; int paused; } ProgressJob;
static void *progress_report_thread(void *arg)
{
    ProgressJob *j = (ProgressJob *)arg;
    jf_report_progress(&g_cfg, j->item_id, j->session, j->pos, j->paused);
    free(j);
    return NULL;
}
static void report_progress_async(const char *item_id, const char *session,
                                   int64_t pos, int paused)
{
    ProgressJob *j = malloc(sizeof(*j));
    if (!j) return;   /* skip one report rather than block the UI */
    snprintf(j->item_id, sizeof(j->item_id), "%s", item_id);
    snprintf(j->session, sizeof(j->session), "%s", session);
    j->pos = pos; j->paused = paused;
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, progress_report_thread, j) != 0) free(j);
    pthread_attr_destroy(&at);
}
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
/* -1 = let the server choose the source's default, otherwise the
 * JfAudioTrack.index baked into the playing stream's URL. Unlike a text
 * subtitle this can't be switched client-side — the server transcodes one
 * chosen audio stream into the container it sends, so changing it is a
 * stream restart (see submenu_confirm). Read by play() on every (re)start so
 * the choice survives seeks, same as the two subtitle variables above. */
static int     g_current_audio_index = -1;
/* Seek is a full stop+restart (network stream isn't byte-range seekable —
 * see player_seek) which takes a couple of seconds; accumulate repeated
 * presses into one seek instead of firing a restart per press (a proven
 * local-seek debounce pattern). */
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
/* Assembled from the config each time rather than being a constant, so a
 * resolution can be tried on hardware by editing jellyfin.conf instead of
 * rebuilding and reflashing per data point. Defaults to the values above when
 * the config says nothing. */
static JfStreamProfile stream_profile(void)
{
    JfStreamProfile p;
    p.max_width     = g_cfg.profile_width;
    p.max_height    = g_cfg.profile_height;
    p.video_bitrate = g_cfg.profile_bitrate;
    return p;
}

static void mp_cmd(const char *cmd)
{
    if (g_cmd_fd >= 0) write(g_cmd_fd, cmd, strlen(cmd));
}

static double play_position(void)
{
    if (g_paused) return g_play_offset + (g_pause_wall - g_play_start_wall);
    return g_play_offset + (now_sec() - g_play_start_wall);
}

/* A title counts as watched once playback passed ~90% of its runtime — the
 * same threshold Jellyfin's own server uses to flip Played and drop the
 * resume marker. Reaching end-of-file lands here too (position ~= runtime).
 * An early stop below the threshold keeps a real resume point instead; an
 * item with no runtime metadata (runtime_ticks==0) can't be judged, so it's
 * treated as not-yet-watched rather than guessed. */
static int playback_watched(int64_t runtime_ticks, double pos_sec)
{
    if (runtime_ticks <= 0) return 0;
    return pos_sec >= 0.9 * (double)runtime_ticks / 10000000.0;
}

static int player_running(void)
{
    if (g_player_pid < 0) return 0;
    if (g_curl_pid > 0) {
        int cstatus;
        if (waitpid(g_curl_pid, &cstatus, WNOHANG) > 0) {
            g_curl_pid = -1;
            if (!(WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0)) {
                /* curl died (bad TLS, DNS, connection refused, ...) — mplayer
                 * is left blocked opening/reading a FIFO with no writer left
                 * and would otherwise hang on it forever. Tear the whole
                 * subtree down now, exactly like mplayer exiting on its
                 * own — SIGKILL reaches it even mid-open(). */
                kill(-g_player_pid, SIGKILL);
                waitpid(g_player_pid, NULL, 0);
                g_player_pid = -1;
                return 0;
            }
            /* curl finished (exit 0) — the whole stream is now sitting in
             * the FIFO/mplayer's own cache; let mplayer play it out and hit
             * EOF on its own below, same as any other title ending. */
        }
    }
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
    if (g_curl_pid > 0) {
        /* Its own process, not in mplayer's group (see stream_via_curl_if_
         * https) — killed explicitly so a stop/seek-restart never leaves it
         * holding the HTTPS connection open. */
        kill(g_curl_pid, SIGKILL);
        waitpid(g_curl_pid, NULL, 0);
        g_curl_pid = -1;
    }
    /* SIGKILL means mplayer's own uninit page restore never ran — put the
     * display back on page 0 and wake Main up (no-op unless engaged). */
    pageflip_end();
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

/* mplayer's bundled FFmpeg has no TLS backend (confirmed on hardware:
 * "https protocol not found, recompile FFmpeg with openssl, gnutls or
 * securetransport enabled" — issue reported against a Jellyfin server
 * behind a reverse proxy/Cloudflare), so an https:// stream can't be handed
 * to it directly. curl already has a working TLS stack for every other
 * Jellyfin API call this app makes, so it fetches the stream instead and
 * writes it into a FIFO; mplayer is pointed at that FIFO path in place of
 * the URL and never knows the difference. http:// streams are left
 * completely untouched by this — the new code path only ever runs for
 * users who currently have nothing working over HTTPS, so it can't
 * regress the months-proven plain-HTTP path.
 *
 * Rewrites `url` in place on success. On any setup failure (mkfifo, fork)
 * it leaves `url` alone, so playback falls back to handing mplayer the
 * https:// URL directly — exactly the same failure this already has today,
 * not a new one. */
static void stream_via_curl_if_https(char *url, size_t url_len)
{
    if (strncasecmp(url, "https://", 8) != 0) return;

    unlink(STREAM_FIFO_PATH);
    if (mkfifo(STREAM_FIFO_PATH, 0600) != 0) return;

    pid_t pid = jf_spawn_stream_curl(&g_cfg, url, STREAM_FIFO_PATH);
    if (pid <= 0) { unlink(STREAM_FIFO_PATH); return; }

    g_curl_pid = pid;
    snprintf(url, url_len, "%s", STREAM_FIFO_PATH);
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
        /* Under page flipping the display is likely parked on page 1 (the
         * player's flip page) — our pause/submenu overlays draw into page
         * 0 (fb->mem), so bring that on screen. The shown frame may be one
         * frame behind the very last decoded one; invisible in practice.
         * On unpause the player's next flip takes over again. */
        if (g_pageflip_engaged) fb_page_flip(0);
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
    subtitles_sanitize_srt(SUB_LOCAL_PATH);

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

/* Track picker, opened with SELECT during playback.
 *
 * Two tabs, because audio and subtitles are genuinely different operations
 * rather than two lists of the same kind: a text subtitle is rendered
 * client-side and switches instantly, whereas the audio track is baked into
 * the server's transcode and switching it costs a stream restart. Splitting
 * them also keeps each list short enough to fit a 240-line NTSC screen,
 * which one combined list of up to 17 rows would not.
 *
 * Cycling with an immediate apply per SELECT press was tried first and caused
 * a cascade when a burn-in restart was still in flight (each press interrupted
 * the previous restart before it connected, showing as a freeze) — hence a
 * menu with an explicit apply, even though a text-subtitle change is instant. */
#define SUBMENU_TAB_AUDIO   0
#define SUBMENU_TAB_SUBS    1
#define SUBMENU_TAB_PICTURE 2
#define SUBMENU_TAB_COUNT   3

static int    g_submenu_visible    = 0;
static int    g_submenu_tab        = SUBMENU_TAB_SUBS;
static int    g_submenu_sub_sel    = 0;   /* 0 = Off, i+1 = g_info_item.subs[i] */
static int    g_submenu_audio_sel  = 0;   /* index into g_info_item.audio[] */
static int    g_submenu_pic_sel    = 0;   /* 0 = Normal, 1 = the contextual zoom (see g_zoom_mode) */
static int    g_submenu_was_paused = 0;   /* pause state before the menu opened, to restore on close */

/* Picture mode (the submenu's PICTURE tab, wide files only — a genuine 4:3
 * file already fills the 4:3 screen, so the tab isn't even shown for one):
 *
 *   0  Default — present the file exactly as encoded (the only behavior
 *      before this existed).
 *   1  Zoom 4:3 — take the central 4:3 region at full height. Made for 4:3
 *      content pillarboxed inside a 16:9 encode (the reported real-world
 *      case: baked black side bars), where it cuts ONLY the bars.
 *   2  Stretch — fill the screen by stretching a genuinely wide picture
 *      vertically (the classic TV "full/stretch" mode: no picture lost,
 *      geometry distorted instead).
 *
 * Deliberately NO pixel-analysis auto-detection of which one a file needs:
 * a dark scene is indistinguishable from a baked bar, so the user's eye is
 * the judge — default off, effect instantly visible, instantly reversible.
 * Applied by restarting the stream at the current position (same mechanism
 * as an audio-track switch); reset to Default whenever a DIFFERENT title
 * starts (see play()), never persisted. */
static int    g_zoom_mode = 0;
#define PICTURE_DAR_WIDE 1.45   /* above = wide file, below = 4:3-ish (1.33 4:3, 1.37 Academy) */

/* The playing item's display aspect ratio, with the same fallback ladder
 * play()'s -vf math uses (source_aspect, raw dimensions, then a plain 16:9
 * guess) — the PICTURE tab's zoom label and play()'s chain must agree on
 * whether a file counts as wide, so they share this. */
static double item_dar(void)
{
    double dar = g_info_item.source_aspect;
    if (dar <= 0.0 && g_info_item.source_width > 0 && g_info_item.source_height > 0)
        dar = (double)g_info_item.source_width / (double)g_info_item.source_height;
    if (dar <= 0.0) dar = 16.0 / 9.0;
    return dar;
}
/* When the menu was opened, so a duplicate of the opening press can't close
 * it again — see the close handling in the main loop. */
static double g_submenu_opened_at = 0.0;
#define SUBMENU_IGNORE_CLOSE_SEC 0.30

/* Which audio track the server would pick if we said nothing — used to show
 * a meaningful "current" marker before the user has chosen anything, and as
 * the starting value for g_current_audio_index. */
static int default_audio_index(void)
{
    for (int i = 0; i < g_info_item.audio_count; i++)
        if (g_info_item.audio[i].is_default) return g_info_item.audio[i].index;
    return g_info_item.audio_count > 0 ? g_info_item.audio[0].index : -1;
}

/* Position in subs[]/audio[] for a given MediaStreams index, or -1.
 *
 * These two are NOT interchangeable: stream indices are assigned by the
 * server across ALL streams of the file, so the first subtitle of a
 * video+audio+2×subtitle file is index 2, not 0. Conflating them is why the
 * "currently active" marker used to point at the wrong row (or at no row at
 * all) for any file whose subtitle streams didn't happen to start at 0. */
static int sub_pos_for_index(int index)
{
    for (int i = 0; i < g_info_item.sub_count; i++)
        if (g_info_item.subs[i].index == index) return i;
    return -1;
}

static int audio_pos_for_index(int index)
{
    for (int i = 0; i < g_info_item.audio_count; i++)
        if (g_info_item.audio[i].index == index) return i;
    return -1;
}

/* Rows in the currently shown tab. */
static int submenu_option_count(void)
{
    if (g_submenu_tab == SUBMENU_TAB_AUDIO)
        return g_info_item.audio_count;      /* no "off" — video always has audio */
    if (g_submenu_tab == SUBMENU_TAB_PICTURE)
        return 3;                            /* Default / Zoom 4:3 / Stretch */
    return g_info_item.sub_count + 1;        /* + "Off" */
}

/* PICTURE is the last tab and only exists for wide files — a genuine 4:3
 * file already fills the 4:3 screen, so offering zoom/stretch there would
 * be nothing but a way to damage a correct picture. */
static int submenu_tab_count(void)
{
    return item_dar() > PICTURE_DAR_WIDE ? SUBMENU_TAB_COUNT
                                          : SUBMENU_TAB_COUNT - 1;
}

/* The frame the menu is drawn over, captured once when it opens.
 *
 * draw_submenu runs every tick and composites into fb->back, which is never
 * otherwise reset — so each frame paints over the last one's leftovers. That
 * goes unnoticed while the box keeps the same dimensions, but the two tabs
 * are different sizes (the audio tab has no sync line, and the lists differ
 * in length), so switching from the taller one left its extra rows stranded
 * on screen outside the new, shorter box: dead text with nothing behind it.
 * Restoring this backdrop before each draw makes every frame a clean
 * composite rather than an accumulation. */
static uint8_t *g_submenu_bg = NULL;
static size_t   g_submenu_bg_size = 0;

static void submenu_bg_capture(FBDev *fb)
{
    size_t sz = (size_t)fb->stride * fb->height;
    if (g_submenu_bg_size != sz) {
        free(g_submenu_bg);
        g_submenu_bg = malloc(sz);
        g_submenu_bg_size = g_submenu_bg ? sz : 0;
    }
    /* Snapshot via the logical back buffer (fb_sync_back below fills it from
     * the screen) rather than raw fb->mem — the two differ in size under
     * line doubling. */
    if (g_submenu_bg) memcpy(g_submenu_bg, fb->back, sz);
}

static void submenu_bg_restore(FBDev *fb)
{
    if (g_submenu_bg && g_submenu_bg_size == (size_t)fb->stride * fb->height)
        memcpy(fb->back, g_submenu_bg, g_submenu_bg_size);
}

static void submenu_open(FBDev *fb)
{
    g_submenu_visible = 1;

    int sub_pos = sub_pos_for_index(g_current_sub_index);
    g_submenu_sub_sel = (g_current_sub_index < 0 || sub_pos < 0) ? 0 : sub_pos + 1;

    int audio_pos = audio_pos_for_index(g_current_audio_index);
    g_submenu_audio_sel = audio_pos < 0 ? 0 : audio_pos;

    g_submenu_pic_sel = g_zoom_mode;
    /* The PICTURE tab can be left as the remembered tab from a wide title —
     * for a 4:3 one it doesn't exist (see submenu_tab_count). */
    if (g_submenu_tab >= submenu_tab_count())
        g_submenu_tab = SUBMENU_TAB_SUBS;

    /* Open on whichever tab is actually useful. Subtitles stay the default
     * (that's what SELECT has always opened), but on a title with no subtitle
     * tracks at all and a real choice of audio, opening on an empty list
     * would look broken. */
    if (g_info_item.sub_count == 0 && g_info_item.audio_count > 1)
        g_submenu_tab = SUBMENU_TAB_AUDIO;

    g_submenu_was_paused = g_paused;
    g_submenu_opened_at  = now_sec();
    if (!g_paused) player_pause_toggle();
    fb_sync_back(fb);        /* pull the live video frame into fb->back... */
    submenu_bg_capture(fb);  /* ...and snapshot it from there (see its comment) */
}

static void submenu_close(void)
{
    g_submenu_visible = 0;
    free(g_submenu_bg);
    g_submenu_bg = NULL;
    g_submenu_bg_size = 0;
    if (!g_submenu_was_paused && g_paused) player_pause_toggle();
}

static void submenu_switch_tab(int tab)
{
    if (tab == g_submenu_tab) return;
    g_submenu_tab = tab;
}

/* One mini screen of the PICTURE tab's card art, transcribed from the
 * user's supplied icons: a white-framed screen, black bars, cyan picture,
 * and a yellow "sun" so the after-state visibly shows the same scene
 * bigger. 17*s tall; the WIDTH comes from par_correction so the frame
 * actually LOOKS 4:3 on screen (this platform's pixels aren't square —
 * drawn at the art's raw 22x17 it read as visibly too tall, per user).
 * state: 0 = letterboxed (bars top/bottom), 1 = windowboxed (bars all
 * around), 2 = full bleed after a zoom (sun stays SQUARE on screen — zoom
 * only enlarges), 3 = blank (the "original/as the source is" screen — no
 * picture-shape claim, per the user's own art for it), 4 = full bleed
 * after a stretch (sun keeps its width but grows visibly TALLER — exactly
 * the distortion stretch costs, and what keeps the two after-states
 * distinguishable at a glance). */
static int pic_mini_screen_w(FBDev *fb, int s)
{
    return (int)((4.0 / 3.0) * par_correction(fb) * 17 * s + 0.5);
}

static void pic_mini_screen(FBDev *fb, int x, int y, int s, int state)
{
    int w  = pic_mini_screen_w(fb, s);
    int h  = 17 * s;
    int ix = x + s, iy = y + s;
    int iw = w - 2 * s, ih = h - 2 * s;
    /* Horizontal art unit: the inner width split into the same 20 columns
     * the original 22x17 art used, so positions transcribe 1:1. */
    #define PMS_X(u) (ix + iw * (u) / 20)

    fb_fill_rect_alpha(fb, x, y, w, h, 0xE8, 0xE8, 0xE8, 255);
    fb_fill_rect_alpha(fb, ix, iy, iw, ih, 0x10, 0x10, 0x10, 255);
    if (state == 3) return;

    /* Sun sizes, in on-screen-square terms: width = height * par. */
    int sun_small_h = 4 * s, sun_small_w = (int)(sun_small_h * par_correction(fb) + 0.5);
    int sun_big_h   = 7 * s, sun_big_w   = (int)(sun_big_h * par_correction(fb) + 0.5);

    if (state == 0) {
        fb_fill_rect_alpha(fb, ix, y + 4 * s, iw, 9 * s, 0x38, 0xC4, 0xF0, 255);
        fb_fill_rect_alpha(fb, x + (w - sun_small_w) / 2, y + 6 * s,
                           sun_small_w, sun_small_h, 0xF0, 0xD8, 0x30, 255);
    } else if (state == 1) {
        fb_fill_rect_alpha(fb, PMS_X(4), y + 4 * s, PMS_X(16) - PMS_X(4), 9 * s,
                           0x38, 0xC4, 0xF0, 255);
        fb_fill_rect_alpha(fb, x + (w - sun_small_w) / 2, y + 6 * s,
                           sun_small_w, sun_small_h, 0xF0, 0xD8, 0x30, 255);
    } else if (state == 2) {
        fb_fill_rect_alpha(fb, ix, iy, iw, ih, 0x38, 0xC4, 0xF0, 255);
        fb_fill_rect_alpha(fb, x + (w - sun_big_w) / 2, iy + (ih - sun_big_h) / 2,
                           sun_big_w, sun_big_h, 0xF0, 0xD8, 0x30, 255);
    } else {
        /* stretched: the zoom sun's width, ~double its height */
        int sun_h = 13 * s;
        fb_fill_rect_alpha(fb, ix, iy, iw, ih, 0x38, 0xC4, 0xF0, 255);
        fb_fill_rect_alpha(fb, x + (w - sun_big_w) / 2, iy + (ih - sun_h) / 2,
                           sun_big_w, sun_h, 0xF0, 0xD8, 0x30, 255);
    }
    #undef PMS_X
}

/* The PICTURE tab: three cards (Default / Zoom 4:3 / Stretch) instead of
 * text rows — each zoom card carries the user's full before->after icon
 * art, because the icons ARE the explanation of what each mode does. */
static void draw_submenu_picture(FBDev *fb)
{
    int box_w = 470, card_w = 138, card_h = 66, card_gap = 8;
    int chrome_h = 10 + 15 + 8 + 3 * 12 + 8;
    int box_h = chrome_h + card_h + 6;
    int box_x = (fb->width - box_w) / 2, box_y = fb->height / 2 - box_h / 2;
    fb_fill_rect_alpha(fb, box_x, box_y, box_w, box_h, 0, 0, 0, 225);

    int list_x = box_x + 12, list_y = box_y + 10;

    static const char *const tab_labels[SUBMENU_TAB_COUNT] = {
        [SUBMENU_TAB_AUDIO]   = "AUDIO",
        [SUBMENU_TAB_SUBS]    = "SUBTITLES",
        [SUBMENU_TAB_PICTURE] = "PICTURE",
    };
    int x = list_x;
    for (int t = 0; t < submenu_tab_count(); t++) {
        if (t == g_submenu_tab) draw_text(fb, x, list_y, tab_labels[t], 1, COL_TITLE);
        else                    draw_text(fb, x, list_y, tab_labels[t], 1, COL_HINT);
        x += text_width(fb, tab_labels[t], 1) + 3 * 8;
    }
    list_y += 15;

    static const char *const captions[3] = { "Original", "Zoom 4:3", "Stretch" };
    int cards_x = box_x + (box_w - 3 * card_w - 2 * card_gap) / 2;
    for (int m = 0; m < 3; m++) {
        int cx = cards_x + m * (card_w + card_gap);
        int cy = list_y;
        int is_sel    = (m == g_submenu_pic_sel);
        int is_active = (m == g_zoom_mode);

        fb_fill_rect_alpha(fb, cx, cy, card_w, card_h, 0x18, 0x18, 0x18, 255);
        if (is_sel) {
            /* Same selection color the list rows use, as a 2px border. */
            fb_fill_rect_alpha(fb, cx, cy, card_w, 2, COL_SEL_BG, 255);
            fb_fill_rect_alpha(fb, cx, cy + card_h - 2, card_w, 2, COL_SEL_BG, 255);
            fb_fill_rect_alpha(fb, cx, cy, 2, card_h, COL_SEL_BG, 255);
            fb_fill_rect_alpha(fb, cx + card_w - 2, cy, 2, card_h, COL_SEL_BG, 255);
        }

        /* Art row: Original is the single as-encoded screen; the zoom cards
         * are before -> after. s=1 keeps a pair inside one card. */
        int art_y = cy + 8;
        int sw = pic_mini_screen_w(fb, 1);
        if (m == 0) {
            pic_mini_screen(fb, cx + (card_w - sw) / 2, art_y + 8, 1, 3);
        } else {
            int arrow_w = 14;
            int pair_w = sw + arrow_w + sw;
            int px = cx + (card_w - pair_w) / 2;
            pic_mini_screen(fb, px, art_y + 8, 1, m == 1 ? 1 : 0);
            draw_text(fb, px + sw + 3, art_y + 8 + 5, ">", 1, COL_ITEM);
            pic_mini_screen(fb, px + sw + arrow_w, art_y + 8, 1, m == 1 ? 2 : 4);
        }

        char cap[16];
        snprintf(cap, sizeof(cap), "%s%s", is_active ? "> " : "", captions[m]);
        int cap_x = cx + (card_w - text_width(fb, cap, 1)) / 2;
        if (is_active)      draw_text(fb, cap_x, cy + card_h - 12, cap, 1, COL_RESUME);
        else if (is_sel)    draw_text(fb, cap_x, cy + card_h - 12, cap, 1, COL_SEL_FG);
        else                draw_text(fb, cap_x, cy + card_h - 12, cap, 1, COL_ITEM);
    }
    list_y += card_h + 6 + 8;

    const char *hint1 = "LEFT/RIGHT: select picture mode";
    const char *hint2 = "L/R: switch tab";
    const char *hint3 = "B: apply    A: cancel";
    draw_text(fb, box_x + (box_w - text_width(fb, hint1, 1)) / 2, list_y, hint1, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(fb, hint2, 1)) / 2, list_y, hint2, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(fb, hint3, 1)) / 2, list_y, hint3, 1, COL_HINT);

    fb_flip(fb);
}

static void draw_submenu(FBDev *fb)
{
    /* Start from the captured backdrop every time — see g_submenu_bg. */
    submenu_bg_restore(fb);

    if (g_submenu_tab == SUBMENU_TAB_PICTURE) {
        draw_submenu_picture(fb);
        return;
    }

    int is_audio = (g_submenu_tab == SUBMENU_TAB_AUDIO);
    int n_opts   = submenu_option_count();
    int sel      = is_audio ? g_submenu_audio_sel : g_submenu_sub_sel;

    /* At least one row of vertical space even when a tab has no tracks, so
     * the "none" message below has somewhere to go. */
    int n_rows   = n_opts > 0 ? n_opts : 1;
    /* The sync readout is subtitle-only — it adjusts subtitle timing, and
     * there's no audio/picture equivalent worth the row. */
    int extra_rows = (g_submenu_tab == SUBMENU_TAB_SUBS) ? 1 : 0;

    /* Wide enough for a full server-composed audio DisplayTitle — "English -
     * Dolby Digital 5.1 - Default" is 37 characters, and at 8px per glyph
     * plus the box's own margins that needs ~340. Anything narrower truncates
     * exactly the suffix that distinguishes a commentary track from the main
     * mix, which is the whole reason for showing DisplayTitle rather than a
     * bare language. Still leaves a comfortable margin at 640 wide. */
    int box_w = 360;

    /* Everything in the box that isn't a track row: tab header, optional sync
     * line, gap, 3 hint lines, padding. Kept as one expression so the height
     * below can't drift out of sync with what's actually drawn — which it did
     * once before, spilling text past the box's own bottom edge. */
    int chrome_h = 10 + 15 + extra_rows * 15 + 8 + 3 * 12 + 8;

    /* The full list doesn't always fit. Eight subtitle tracks plus "Off" is
     * JF_MAX_SUBS's worst case, and at NTSC's 240 lines that box overflows
     * the overscan-safe area at both ends — on a real CRT the first and last
     * rows would simply be off the tube. Cap the rows to what fits inside the
     * safe area and scroll the list instead, so no row is ever undisplayable. */
    int avail_h  = fb->height - 2 * SAFE_Y;
    int max_rows = (avail_h - chrome_h) / 15;
    if (max_rows < 1) max_rows = 1;
    int shown = n_rows < max_rows ? n_rows : max_rows;

    /* Scroll offset per tab, nudged just far enough to keep the selection on
     * screen — same minimal-movement behaviour as the browse list, rather
     * than re-centring on every keypress. */
    static int submenu_scroll[SUBMENU_TAB_COUNT] = { 0 };
    int *scroll = &submenu_scroll[g_submenu_tab];
    if (sel < *scroll)          *scroll = sel;
    if (sel >= *scroll + shown) *scroll = sel - shown + 1;
    if (*scroll > n_opts - shown) *scroll = n_opts - shown;
    if (*scroll < 0)              *scroll = 0;

    int box_h = chrome_h + shown * 15;
    int box_x = (fb->width - box_w) / 2, box_y = fb->height / 2 - box_h / 2;
    fb_fill_rect_alpha(fb, box_x, box_y, box_w, box_h, 0, 0, 0, 225);

    int list_x = box_x + 12, list_y = box_y + 10;
    int label_max_w = box_w - 24;   /* box_w minus left/right margin, for truncation below */

    /* Tab header — the inactive tabs stay visible (dimmed) rather than being
     * hidden, so there's something on screen telling you the others are
     * there and that L/R reaches them. */
    {
        static const char *const tab_labels[SUBMENU_TAB_COUNT] = {
            [SUBMENU_TAB_AUDIO]   = "AUDIO",
            [SUBMENU_TAB_SUBS]    = "SUBTITLES",
            [SUBMENU_TAB_PICTURE] = "PICTURE",
        };
        int gap = 3 * 8;
        int x = list_x;
        for (int t = 0; t < submenu_tab_count(); t++) {
            if (t == g_submenu_tab) draw_text(fb, x, list_y, tab_labels[t], 1, COL_TITLE);
            else                    draw_text(fb, x, list_y, tab_labels[t], 1, COL_HINT);
            x += text_width(fb, tab_labels[t], 1) + gap;
        }
        /* Only when some of the list is off-screen — otherwise it's noise. */
        if (shown < n_opts) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", sel + 1, n_opts);
            draw_text(fb, box_x + box_w - 12 - text_width(fb, pos, 1),
                      list_y, pos, 1, COL_HINT);
        }
        list_y += 15;
    }

    if (n_opts == 0) {
        draw_text(fb, list_x, list_y, "  (no tracks)", 1, COL_HINT);
        list_y += 15;
    }

    for (int i = *scroll; i < n_opts && i < *scroll + shown; i++) {
        const char *label;
        int active;

        if (is_audio) {
            const JfAudioTrack *a = &g_info_item.audio[i];
            label  = a->label[0] ? a->label : "Unknown";
            active = (g_current_audio_index == a->index);
        } else {
            int is_off = (i == 0);
            label  = is_off ? "Off" : g_info_item.subs[i - 1].label;
            active = is_off ? (g_current_sub_index < 0)
                            : (g_current_sub_index == g_info_item.subs[i - 1].index);
            if (!label || !label[0]) label = "Unknown";
        }

        if (i == sel)
            fb_fill_rect_alpha(fb, box_x + 4, list_y - 2, box_w - 8, 14, COL_SEL_BG, 220);

        char line[80];
        snprintf(line, sizeof(line), "%s%s", active ? "> " : "  ", label);
        /* A long DisplayTitle from the server (seen in practice: things like
         * "Undefined - SUBRIP - External", or "English - Dolby Digital 5.1 -
         * Default" for audio) drawn past the box's own width made the text
         * stick out past the dark background behind it — clip it to fit. */
        truncate_to_width(fb, line, 1, label_max_w);
        if (active) draw_text(fb, list_x, list_y, line, 1, COL_RESUME);
        else        draw_text(fb, list_x, list_y, line, 1, COL_ITEM);
        list_y += 15;
    }

    if (g_submenu_tab == SUBMENU_TAB_SUBS) {
        char syncline[32];
        snprintf(syncline, sizeof(syncline), "Sync: %+.1fs", g_sub_delay_extra);
        draw_text(fb, list_x, list_y, syncline, 1, COL_ITEM);
        list_y += 15;
    }
    list_y += 8;   /* gap before the hint block */

    const char *hint1 = is_audio ? "UP/DOWN: select audio track"
                                  : "UP/DOWN: select subtitle";
    const char *hint2 = (g_submenu_tab == SUBMENU_TAB_SUBS)
                          ? "L/R: switch tab   LEFT/RIGHT: sync"
                          : "L/R: switch tab";
    const char *hint3 = "B: apply    A: cancel";
    draw_text(fb, box_x + (box_w - text_width(fb, hint1, 1)) / 2, list_y, hint1, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(fb, hint2, 1)) / 2, list_y, hint2, 1, COL_HINT);
    list_y += 12;
    draw_text(fb, box_x + (box_w - text_width(fb, hint3, 1)) / 2, list_y, hint3, 1, COL_HINT);

    fb_flip(fb);
}

static void submenu_confirm(FBDev *fb)
{
    int new_sub = (g_submenu_sub_sel == 0 || g_info_item.sub_count == 0)
                    ? -1 : g_info_item.subs[g_submenu_sub_sel - 1].index;
    int new_audio = (g_info_item.audio_count > 0)
                    ? g_info_item.audio[g_submenu_audio_sel].index
                    : g_current_audio_index;
    int new_zoom = g_submenu_pic_sel;

    submenu_close();

    /* An audio change can only be applied by rebuilding the stream URL, and
     * a picture-zoom change by rebuilding the -vf chain — either way the
     * stream restarts, so one restart subsumes every change made in the
     * same visit (including a subtitle change) rather than restarting
     * twice. */
    if (new_audio != g_current_audio_index || new_zoom != g_zoom_mode) {
        double pos = play_position();
        g_current_audio_index = new_audio;
        g_zoom_mode           = new_zoom;

        int new_burn_in = -1;
        if (new_sub >= 0) {
            const JfSubtitle *s = find_sub(new_sub);
            if (s && !jf_subtitle_is_text(s->codec)) new_burn_in = new_sub;
        }
        g_burned_in_sub_index = new_burn_in;
        g_current_sub_index   = new_sub;

        player_stop();
        play(fb, g_info_item.id, pos);   /* re-loads a client-side subtitle itself */
        return;
    }

    if (new_sub != g_current_sub_index) subtitle_apply(fb, new_sub);
}

/* The submenu's whole input frame, lifted verbatim from the main loop so
 * the menu's state stays module-internal. Every path used to end in
 * `continue`; the caller preserves that by `continue`-ing right after this
 * returns, so the confirm/close paths still skip the usleep exactly as
 * before (their input_drain + immediate next poll was deliberate).  */
static void submenu_handle_input(FBDev *fb, int inp, int nav_repeat, double loop_now)
{
    static double last_nav_press = 0.0;
    int n_opts   = submenu_option_count();
    int is_audio = (g_submenu_tab == SUBMENU_TAB_AUDIO);
    int is_subs  = (g_submenu_tab == SUBMENU_TAB_SUBS);
    int is_pic   = (g_submenu_tab == SUBMENU_TAB_PICTURE);
    int *sel     = is_audio ? &g_submenu_audio_sel :
                   is_subs  ? &g_submenu_sub_sel   : &g_submenu_pic_sel;

    /* Shoulder buttons walk the tab strip (AUDIO | SUBTITLES | PICTURE —
     * the last one only for wide files, see submenu_tab_count), leaving
     * LEFT/RIGHT free for subtitle sync (which needs to stay a fine
     * repeated nudge) and for the picture tab's card selection. */
    if (inp & INP_L && g_submenu_tab > 0)
        submenu_switch_tab(g_submenu_tab - 1);
    if (inp & INP_R && g_submenu_tab < submenu_tab_count() - 1)
        submenu_switch_tab(g_submenu_tab + 1);

    int menu_inp = inp | nav_repeat;   /* held UP/DOWN walks the list, held LEFT/RIGHT keeps nudging sync */
    if ((menu_inp & (INP_UP | INP_DOWN | INP_LEFT | INP_RIGHT)) &&
        loop_now - last_nav_press > 0.15) {
        last_nav_press = loop_now;
        if (menu_inp & INP_UP)    { if (*sel > 0) (*sel)--; }
        if (menu_inp & INP_DOWN)  { if (*sel < n_opts - 1) (*sel)++; }
        /* The picture tab's cards sit side by side, so LEFT/RIGHT is the
         * natural selector there (UP/DOWN above still works too). */
        if (is_pic && (menu_inp & INP_LEFT))  { if (*sel > 0) (*sel)--; }
        if (is_pic && (menu_inp & INP_RIGHT)) { if (*sel < n_opts - 1) (*sel)++; }
        /* Live-tunable on top of the fixed baseline (AUDIO_DELAY_SEC
         * + SUBTITLE_SYNC_FUDGE_SEC + g_play_offset) — for whatever
         * that fixed default doesn't cover on a specific subtitle
         * file. Applies immediately if a subtitle is already
         * loaded. Subtitle tab only: there's nothing for it to mean
         * on the other tabs, and silently changing subtitle timing
         * from a screen not showing it would be a surprise. */
        if (is_subs && (menu_inp & INP_LEFT))  { g_sub_delay_extra -= 0.1; sub_delay_send(); }
        if (is_subs && (menu_inp & INP_RIGHT)) { g_sub_delay_extra += 0.1; sub_delay_send(); }
    }
    if (inp & INP_A) { submenu_confirm(fb); input_drain(); return; }
    /* SELECT also closes — a SELECT press while already open was a
     * silent no-op before (it only opens from the STATE_PLAYING
     * switch below, which this block's own `continue` never
     * reaches while visible), so a second SELECT looked like "the
     * menu doesn't open". B still means an explicit cancel too.
     *
     * Ignored for a moment after opening, though. The press that
     * opens this menu can be reported twice — a pad that enumerates
     * as several event nodes, or MiSTer's own OSD echoing physical
     * input onto a second virtual device, both do that — and if the
     * duplicate lands in a later poll than the original it arrives
     * here and closes the menu immediately. Draining on open only
     * discards what was already queued at that instant, so a
     * duplicate a few milliseconds behind still gets through; the
     * menu then flickers open and shut and reads as "the button
     * doesn't reliably work". */
    if ((inp & (INP_B | INP_SELECT)) &&
        loop_now - g_submenu_opened_at > SUBMENU_IGNORE_CLOSE_SEC) {
        submenu_close(); input_drain(); return;
    }
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
    unsigned long flips_before = g_fb_flip_count;
    draw_submenu(fb);
    if (g_fb_flip_count == flips_before) usleep(16000);
    return;
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

/* ── session message banner ───────────────────────────────────────────────
 * An admin message from the Jellyfin dashboard (SESSION_EV_MESSAGE): a blue
 * container rises from the bottom edge, the message scrolls across it right
 * to left, and once the whole text has left the screen the container sinks
 * back down and disappears. Registered as the fb_flip overlay (see
 * fb_set_overlay) so it plays over every app-drawn screen without each
 * screen's composer knowing about it; every screen redraws at full refresh
 * rate, so the overlay animates smoothly wherever it fires. During VIDEO
 * playback mplayer owns the framebuffer and would repaint over it, so
 * messages arriving then go through mplayer's own OSD instead (osd_flash) —
 * see the session-event handling in the main loop. Main-thread only (the
 * loop enqueues, fb_flip draws), so no locking. */
#define BANNER_QUEUE_MAX 4
#define BANNER_TEXT_MAX  320
#define BANNER_H         24                /* container height: scale-2 text + padding */
#define BANNER_RISE_SECS 0.30
#define BANNER_SPEED     110.0             /* scroll, px/sec */
#define BANNER_BLUE      0x00, 0xA4, 0xDC  /* Jellyfin's own theme blue */

enum { BANNER_IDLE, BANNER_RISE, BANNER_SCROLL, BANNER_FALL };
static char   g_banner_queue[BANNER_QUEUE_MAX][BANNER_TEXT_MAX];
static int    g_banner_qcount;
static char   g_banner_text[BANNER_TEXT_MAX];
static int    g_banner_phase = BANNER_IDLE;
static double g_banner_t0;

static void banner_enqueue(const char *text)
{
    if (g_banner_qcount >= BANNER_QUEUE_MAX) return;   /* someone's spamming — drop */
    snprintf(g_banner_queue[g_banner_qcount++], BANNER_TEXT_MAX, "%s", text);
}

/* During VIDEO playback mplayer owns the framebuffer, so the banner is
 * drawn by our patched vo_fbdev instead (docker/vo_fbdev.c, ban_frame) —
 * same animation, same look. The handoff is this flag file (the
 * /tmp/misterdvd_vsync pattern): written atomically (tmp + rename) so a
 * frame's read can't catch a half-written message; mplayer takes ownership
 * (reads + unlinks) on the next frame. If playback ends before any frame
 * consumes it, the main loop finds the leftover and replays it through the
 * app's own banner (see the pickup next to the session-event handling). */
#define BANNER_FILE "/tmp/misterfin_banner"

static void banner_file_write(const char *text)
{
    char tmp[] = "/tmp/misterfin_banner_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    int ok = (fputs(text, f) >= 0);
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, BANNER_FILE) != 0) unlink(tmp);
}

static void banner_overlay(FBDev *fb)
{
    double now = now_sec();

    if (g_banner_phase == BANNER_IDLE) {
        if (g_banner_qcount == 0) return;
        snprintf(g_banner_text, sizeof(g_banner_text), "%s", g_banner_queue[0]);
        memmove(g_banner_queue[0], g_banner_queue[1],
                sizeof(g_banner_queue[0]) * (BANNER_QUEUE_MAX - 1));
        g_banner_qcount--;
        g_banner_phase = BANNER_RISE;
        g_banner_t0 = now;
    }

    /* The container's resting top edge — its fill always extends to the
     * physical bottom so there's no floating gap, but the text row sits
     * above the overscan margin where a CRT is sure to show it. */
    int rest_top = fb->height - SAFE_Y_BOT - BANNER_H;
    int top;

    if (g_banner_phase == BANNER_RISE) {
        double t = (now - g_banner_t0) / BANNER_RISE_SECS;
        if (t >= 1.0) { g_banner_phase = BANNER_SCROLL; g_banner_t0 = now; t = 1.0; }
        double e = 1.0 - (1.0 - t) * (1.0 - t);   /* ease-out */
        top = fb->height - (int)((fb->height - rest_top) * e);
    } else if (g_banner_phase == BANNER_SCROLL) {
        top = rest_top;
    } else {
        double t = (now - g_banner_t0) / BANNER_RISE_SECS;
        if (t >= 1.0) { g_banner_phase = BANNER_IDLE; return; }
        top = rest_top + (int)((fb->height - rest_top) * t * t);   /* ease-in */
    }

    fb_fill_rect_alpha(fb, 0, top, fb->width, fb->height - top, BANNER_BLUE, 235);

    if (g_banner_phase == BANNER_SCROLL) {
        int tw = text_width(fb, g_banner_text, 2);
        int x  = fb->width - (int)((now - g_banner_t0) * BANNER_SPEED);
        if (x + tw < 0) {   /* whole message has left the screen */
            g_banner_phase = BANNER_FALL;
            g_banner_t0 = now;
            return;
        }
        draw_text_clipped(fb, x, top + (BANNER_H - 16) / 2, g_banner_text, 2,
                          0xFF, 0xFF, 0xFF, 0, fb->width);
    }
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

/* One frame of video-playback input handling, lifted verbatim from the
 * main loop's STATE_PLAYING case. Returns PLAYER_FRAME_PLAYING while the
 * title keeps playing, PLAYER_FRAME_ENDED once it stopped (naturally or
 * via B) with reporting/g_info_item bookkeeping already done — the caller
 * owns the transition back to browse — and PLAYER_FRAME_HANDLED when the
 * frame ended inside the submenu-open path, which must skip the caller's
 * end-of-loop work (a pending accumulated seek must not fire under the
 * just-opened menu, exactly as the original `continue` guaranteed). */
#define PLAYER_FRAME_ENDED   0
#define PLAYER_FRAME_PLAYING 1
#define PLAYER_FRAME_HANDLED 2
static int player_handle_input(FBDev *fb, int inp, double loop_now)
{
    if (!player_running()) {
        /* mplayer exited on its own (end of title) — its uninit
         * already flipped back to page 0; this wakes Main up and
         * clears the flag (no-op unless engaged). */
        pageflip_end();
        double pos = play_position();
        int watched = playback_watched(g_info_item.runtime_ticks, pos);
        jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                           (int64_t)(pos * 10000000.0), watched);
        g_info_item.played = watched;
        g_info_item.resume_ticks = watched ? 0 : (int64_t)(pos * 10000000.0);
        jf_log_line("play: ended on its own at %.0fs, watched=%d", pos, watched);
        return PLAYER_FRAME_ENDED;
    } else if (inp & INP_B) {
        double pos = play_position();
        int watched = playback_watched(g_info_item.runtime_ticks, pos);
        jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                           (int64_t)(pos * 10000000.0), watched);
        g_info_item.played = watched;
        g_info_item.resume_ticks = watched ? 0 : (int64_t)(pos * 10000000.0);
        player_stop();
        jf_log_line("play: stopped by user at %.0fs, watched=%d", pos, watched);
        return PLAYER_FRAME_ENDED;
    } else if (g_paused) {
        if (inp & INP_SELECT) { submenu_open(fb); draw_submenu(fb); input_drain(); return PLAYER_FRAME_HANDLED; }
        if (inp & INP_LEFT)  seek_accumulate(-SEEK_STEP, loop_now);
        if (inp & INP_RIGHT) seek_accumulate(+SEEK_STEP, loop_now);
        if (inp & INP_A) player_pause_toggle();
        if (!g_pageflip_mode) {
            if (inp & INP_L) {
                int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (vf >= 0) close(vf);
                osd_flash("VSync: ON", 1);
            }
            if (inp & INP_R) {
                unlink(VSYNC_FLAG);
                osd_flash("VSync: OFF", 1);
            }
        }
        /* seek_pending_target() reflects any not-yet-fired
         * accumulated seek too, so this timeline moves live as the
         * user taps LEFT/RIGHT instead of waiting for the debounce
         * to actually fire the restart. */
        draw_paused(fb, g_info_item.name, seek_pending_target());
    } else {
        if (inp & INP_SELECT) { submenu_open(fb); draw_submenu(fb); input_drain(); return PLAYER_FRAME_HANDLED; }
        if (inp & INP_A) player_pause_toggle();
        if (inp & INP_LEFT)  seek_accumulate(-SEEK_STEP, loop_now);
        if (inp & INP_RIGHT) seek_accumulate(+SEEK_STEP, loop_now);
        if (!g_pageflip_mode) {
            if (inp & INP_L) {
                int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (vf >= 0) close(vf);
                osd_flash("VSync: ON", 0);
            }
            if (inp & INP_R) {
                unlink(VSYNC_FLAG);
                osd_flash("VSync: OFF", 0);
            }
        }

        if (loop_now - g_last_progress_report >= PROGRESS_REPORT_INTERVAL) {
            g_last_progress_report = loop_now;
            report_progress_async(g_info_item.id, g_play_session_id,
                                  (int64_t)(play_position() * 10000000.0), 0);
        }
    }
    
    return PLAYER_FRAME_PLAYING;
}

static void play(FBDev *fb, const char *item_id, double offset_secs)
{
    g_seek_accum = 0.0;
    g_seek_fire_at = 0.0;

    /* Deliberately does NOT clear /dev/fb0 here — whatever's already on
     * screen (info screen, or the last video frame before a seek-restart)
     * should stay visible behind the loading spinner below. The -vf chain's
     * dsize=<fb dims> (see execlp below) makes mplayer's own frames fill the
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

    char url[700];
    const JfStreamProfile profile = stream_profile();
    jf_stream_url(&g_cfg, item_id, &profile, start_ticks, g_play_session_id,
                  g_burned_in_sub_index, g_current_audio_index, url, sizeof(url));
    stream_via_curl_if_https(url, sizeof(url));
    /* Deliberately no item_id/title/url here — those identify what's in
     * someone's library, not how MiSTerFin behaved. */
    jf_log_line("play: profile=%dx%d@%d fb_phys_h=%d line_double=%d resume=%.0fs",
                profile.max_width, profile.max_height, profile.video_bitrate,
                fb->phys_height, fb->line_double, offset_secs);

    char delay_arg[16];
    snprintf(delay_arg, sizeof(delay_arg), "%.2f", AUDIO_DELAY_SEC);

    g_play_offset     = offset_secs > 0.0 ? offset_secs : 0.0;
    g_play_start_wall = now_sec();
    g_paused          = 0;
    g_last_progress_report = now_sec();
    jf_report_start(&g_cfg, item_id, g_play_session_id, start_ticks, "Transcode");

    /* Picture zoom survives restarts of the SAME title (that's exactly how
     * the PICTURE tab applies it — see submenu_confirm) but never carries
     * over to a different one: whether a crop makes sense is a per-file
     * property of what's baked into its frames. */
    {
        static char zoom_item[JF_ID_LEN] = "";
        if (strcmp(zoom_item, item_id) != 0) {
            g_zoom_mode = 0;
            snprintf(zoom_item, sizeof(zoom_item), "%s", item_id);
        }
    }

    /* Video, unlike the UI, uses the PHYSICAL framebuffer height: mplayer
     * opens /dev/fb0 itself and sees the real line count, and on an
     * interlaced 576/480-line raster the whole point is that video CAN
     * carry that full vertical resolution (the UI's line doubling is a
     * layout decision, not a display limit). vw/vh == fb->width/height
     * everywhere except under line doubling — or under ui_scaled, where
     * they're the 640x480 window fb_open clamped out of the bigger canvas,
     * -geometry'd to its center. Scaling video to the UI's full 960x720
     * box instead was tried and the extra swscale/framebuffer-write load
     * visibly outran this CPU (confirmed on hardware) — the smaller
     * centered window is the deliberate trade; play() blanks the screen
     * first so the UI doesn't linger around it. */
    int vw = fb->width;
    int vh = fb->phys_height;
    char geom_arg[24];
    int  have_geom = 0;
    if (fb->ui_scaled) {
        snprintf(geom_arg, sizeof(geom_arg), "+%d+%d",
                 (fb->real_width - vw) / 2, (fb->real_height - vh) / 2);
        have_geom = 1;
    }

    char vf_arg[128];
    if (vh == 288 && !g_zoom_mode &&
        g_cfg.profile_width == 480 && g_cfg.profile_height == 270) {
        /* PAL at the ORIGINAL 480x270 transcode dimensions (the default for
         * the platform's entire history until the 640x288 bump — see
         * JF_PROFILE_DEFAULT_*) — byte-for-byte the original, long-proven
         * chain, kept selectable by explicitly setting 480x270 in
         * jellyfin.conf. Deliberately NOT touched by the NTSC comb fix
         * below: PAL's width-first "scale=640:-1" mismatch is real
         * (confirmed via the same PAR math used for NTSC) but mild enough
         * to have never shown a visible artifact at these dimensions.
         *
         * Gated on the literal legacy dimensions rather than the current
         * defaults, because "-1" keeps whatever coded height the server
         * sent: at 480x270 that lands close enough, but a wider profile
         * (like the 640x288 default) makes the picture fill the framebuffer
         * with no letterboxing at all, and anything taller than the
         * framebuffer corrupts outright (confirmed on hardware with
         * 640x480: pink garbage / white comb lines, exactly the NTSC
         * failure mode described below). Everything else takes the same
         * DAR-aware branch NTSC uses — including the 720x576 default,
         * confirmed correct on a PAL CRT (letterbox, 4:3, DVD-rip sources,
         * and multi-minute zero-drift/zero-drop soaks). Bitrate-only tweaks
         * on 480x270 stay on this chain. */
        snprintf(vf_arg, sizeof(vf_arg), "scale=%d:-1,expand=%d:%d:-1:-1:1,dsize=%d:%d",
                 vw, vw, vh, vw, vh);
    } else {
        /* NTSC (and any other non-288 height), plus PAL with a custom
         * transcode profile (see above) — confirmed on hardware that
         * "scale=640:-1" (sizing height assuming square pixels) builds an
         * intermediate frame far taller than the framebuffer, which
         * something downstream (dsize/vo_fbdev) then has to crush back
         * down — that crush is the comb/tearing artifact widely reported
         * on NTSC. Scaling directly to the PAR-correct height instead
         * (derived from g_info_item's real source aspect, which
         * jf_get_item_details already fetches for the info screen) gives
         * mplayer a single real swscale pass at the exact final size, so
         * expand only ever pads, never shrinks. Falls back to raw source
         * width/height, then a plain 16:9 guess, if aspect metadata is
         * missing — better than reverting to the confirmed-broken chain. */
        double dar = item_dar();

        /* The 4:3-display height formula below is right for every CRT-class
         * canvas (their pixels aren't square and the screen is 4:3). A
         * square-pixel canvas that ISN'T a 4:3 window — 640x360, i.e. 720p
         * with fb_size=2, where the FPGA scaler upscales the whole fb to
         * the 16:9 output for free — needs a plain both-dimensions fit
         * instead: the old math letterboxed a 16:9 title on it AND showed
         * it display-stretched. Same <360-lines rule as par_correction();
         * for the 4:3 square-pixel canvases this also covers (the 640x480
         * ui_scaled window) the two formulas are algebraically identical
         * (640/dar == 4*480/(3*dar)), so nothing changes there. */
        int square_fit = !fb->line_double && vh >= 360;
        int target_w = vw;
        int target_h;
        if (square_fit) {
            target_h = (int)(vw / dar + 0.5);
            if (target_h > vh) {
                target_h = vh;
                target_w = (int)(vh * dar + 0.5);
            }
        } else {
            target_h = (int)(4.0 * vh / (3.0 * dar) + 0.5);
        }
        target_w &= ~1;                        /* even, required for yuv420p chroma subsampling */
        if (target_w < 2) target_w = 2;
        if (target_w > vw) target_w = vw;
        target_h &= ~1;
        if (target_h < 2) target_h = 2;
        if (target_h > vh) target_h = vh;

        /* The standalone interlaced menu core's raster crops roughly the
         * first 40 physical rows and none at the bottom (measured on
         * hardware with a row-ruler test pattern) — auto-centering
         * (expand's "-1" y-offset) splits the letterbox padding evenly
         * across a range that isn't evenly visible, so the top bar reads
         * visibly shorter than the bottom one and, on a full-height 4:3
         * source, the picture itself runs off the top edge. Push the
         * padding (and so the picture) down by the crop amount instead of
         * auto-centering, so what's actually ON SCREEN is centered within
         * the visible ~[40,vh) window rather than the full [0,vh) buffer.
         * Only applies to this mode (vh is only ever 576/480 here); NTSC's
         * ordinary 240-line progressive path is unaffected. */
        if (g_zoom_mode && square_fit) {
            /* The same two picture modes as the CRT branch below, rebuilt
             * for a square-pixel window (no PAR games, no interlaced crop,
             * and the Original fit already sized BOTH dimensions above):
             * Stretch fills the window in one distorting pass; Zoom 4:3
             * enlarges the Original fit until it covers the window and
             * center-crops the overflow — for a 4:3 picture baked into a
             * 16:9 file that overflow is exactly the black side bars. */
            if (g_zoom_mode == 2) {
                snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,dsize=%d:%d",
                         vw, vh & ~1, vw, vh);
            } else {
                double zw = (double)vw / target_w, zh = (double)vh / target_h;
                double z  = zw > zh ? zw : zh;
                int sw = ((int)(target_w * z + 0.5)) & ~1;
                int sh = ((int)(target_h * z + 0.5)) & ~1;
                if (sw < vw) sw = vw;
                if (sh < (vh & ~1)) sh = vh & ~1;
                snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,crop=%d:%d,dsize=%d:%d",
                         sw, sh, vw, vh & ~1, vw, vh);
            }
            goto vf_done;
        }

        /* !square_fit from here down: this zoom math is built around the
         * 4:3-display width-first fit (z scales off target_h with width
         * pinned at vw) — the square-pixel case took its own branch above. */
        if (g_zoom_mode && !square_fit) {
            /* Picture modes (see g_zoom_mode's comment for what each means
             * and why there's no auto-detection). Under the interlaced
             * core's cropped raster only [40, vh) is visible (see the
             * default branch below) — both modes fill and center within
             * that window, padding the invisible top rows. The vo always
             * receives exactly framebuffer-sized frames, so the NTSC comb
             * failure mode this branch's own comment describes can't
             * re-enter through here. */
            /* interlaced_core, NOT line_double: the 40-row crop is a
             * property of the interlaced core's raster — a progressive
             * HDMI canvas with the same line count shows all vh rows. */
            int usable_h = fb->interlaced_core ? vh - 40 : vh;
            char expand_zoom[48];
            if (fb->interlaced_core)
                snprintf(expand_zoom, sizeof(expand_zoom), "expand=%d:%d:-1:%d:0",
                         vw, vh, 40);
            else
                snprintf(expand_zoom, sizeof(expand_zoom), "expand=%d:%d:-1:-1:1",
                         vw, vh);

            if (g_zoom_mode == 2) {
                /* Stretch: one swscale pass straight to the full screen —
                 * vertical geometry distorts, nothing is cropped. */
                snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,%s,dsize=%d:%d",
                         vw, usable_h & ~1, expand_zoom, vw, vh);
            } else {
                /* Zoom 4:3: scale the normal aspect-correct fit (640 x
                 * target_h) UP so the picture fills the full height, then
                 * center-crop the width back to the screen — crop runs
                 * after scale in the chain, so its arguments are fixed
                 * screen dimensions, not source-dependent. For pillarboxed
                 * 4:3-in-16:9 the crop removes just the baked bars. */
                double z = (double)usable_h / target_h;
                int sw = ((int)(vw * z + 0.5)) & ~1;
                int sh = usable_h & ~1;
                if (sw < vw) sw = vw;
                snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,crop=%d:%d,%s,dsize=%d:%d",
                         sw, sh, vw, sh, expand_zoom, vw, vh);
            }
            goto vf_done;
        }

        char expand_arg[48];
        /* interlaced_core, NOT line_double — same reason as the zoom branch
         * above: only the real interlaced raster crops the top 40 rows. */
        if (fb->interlaced_core) {
            /* Center within the VISIBLE window [40, vh), not the full
             * [0, vh) buffer — the first attempt added 40 on top of a
             * normal full-buffer centering, which overshot downward
             * (confirmed on hardware: top bar became taller than the
             * bottom one, the opposite of the original asymmetry).
             *
             * A near-4:3 source (little/no letterboxing needed) can DAR-
             * compute a target_h tall enough that fitting it starting at
             * row 40 would run past vh — confirmed on hardware with a
             * 1.66:1 title specifically, visibly shifted up into the
             * cropped zone (the "top + target_h > vh" fallback below let
             * top go back under 40). Cap target_h itself to the visible
             * window's height instead, so top can never need to. Costs a
             * few rows of picture on the rare title tall enough to hit
             * this — better than losing them off-screen instead. */
            int visible_h = vh - 40;
            if (target_h > visible_h) target_h = visible_h & ~1;
            int top = 40 + (visible_h - target_h) / 2;
            if (top < 40) top = 40;
            /* expand's trailing arg is its own "osd" flag (confirmed by
             * reading vf_expand.c's option table) — when on (the default,
             * and what every other chain here still uses), expand's
             * control() INTERCEPTS VFCTRL_DRAW_OSD and never forwards it
             * down the chain, instead baking OSD text into the frame
             * itself at coordinates relative to the full pre-crop 640xvh
             * canvas. Confirmed on hardware: a debug counter in vo_fbdev's
             * draw_alpha() stayed at zero through repeated OSD triggers
             * while the message still appeared (baked in by expand) at
             * the top of the frame — inside the cropped zone, invisible
             * for exactly the reason our vo-level clamp never got a
             * chance to run. Turning it off here lets DRAW_OSD reach the
             * vo, where it has the crop margin to correct it. */
            snprintf(expand_arg, sizeof(expand_arg), "expand=%d:%d:-1:%d:0", vw, vh, top);
        } else {
            snprintf(expand_arg, sizeof(expand_arg), "expand=%d:%d:-1:-1:1", vw, vh);
        }
        snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,%s,dsize=%d:%d",
                 target_w, target_h, expand_arg, vw, vh);
    }
vf_done:;

    /* A selected client-rendered (text) subtitle rides the COMMAND LINE
     * (-sub/-subdelay) rather than slave commands sent after the fork:
     * confirmed on hardware (get_property sub_delay) that this mplayer
     * build resets sub_delay to 0 when playback initialization completes,
     * silently wiping any value written into the slave pipe before that
     * point. At offset 0 the wiped value was only the fixed ~1.6s sync
     * correction — subtitles still showed, just slightly off, so nobody
     * noticed — but on a seek/audio-change restart it's the entire
     * -g_play_offset clock shift (minutes), leaving the subtitle "selected
     * but never displayed" (issue #12). Startup options ARE the init
     * values, so nothing wipes them; the slave path (subtitle_load_client)
     * stays for LIVE track changes, which happen with playback long since
     * initialized. An image-based (burned-in) selection needs none of
     * this — it's baked into the url via g_burned_in_sub_index. A failed
     * download just means no -sub args, the same net result as the old
     * post-spawn attempt failing. */
    int cmdline_sub = 0;
    char subdelay_arg[24];
    if (g_current_sub_index >= 0 && g_burned_in_sub_index < 0 &&
        jf_download_subtitle(&g_cfg, g_info_item.id, g_info_item.id,
                             g_current_sub_index, SUB_LOCAL_PATH)) {
        subtitles_sanitize_srt(SUB_LOCAL_PATH);
        snprintf(subdelay_arg, sizeof(subdelay_arg), "%.3f",
                 AUDIO_DELAY_SEC + SUBTITLE_SYNC_FUDGE_SEC - g_play_offset + g_sub_delay_extra);
        cmdline_sub = 1;
    }

    /* Hardware page flipping for the interlaced modes — must be engaged
     * (flag file + Main_MiSTer stopped) before the fork, so the fresh
     * mplayer's vo config sees the flag. See pageflip_begin()'s comment. */
    pageflip_begin();

    int pfd[2];
    pipe(pfd);

    g_player_pid = fork();
    if (g_player_pid == 0) {
        setpgid(0, 0);   /* own process group, so player_stop() can kill mplayer's cache-fill child too */
        /* MiSTer's script launcher pins scripts (and so everything they
         * spawn) to CPU1 — measured on hardware during a full-height
         * (576-line) interlaced page-flip playback: cpu1 ~90% busy, cpu0
         * ~97% IDLE, with visible slow-motion on tall (4:3) pictures. Give
         * mplayer both cores back; its decode already runs threads=2 (see
         * -lavdopts) and the video pipeline gets a core to itself. Raw
         * syscall with a plain bitmask (bit 0 = cpu0, bit 1 = cpu1)
         * instead of the cpu_set_t macros, which need _GNU_SOURCE.
         *
         * ONLY for page-flip mode: Main_MiSTer is SIGSTOPped there, so
         * cpu0 is free. In normal progressive mode Main keeps running and
         * actively uses cpu0 for its own scaler/display duties — letting
         * mplayer spread onto it there caused severe slowdowns (confirmed
         * on hardware), so leave progressive on the launcher's original
         * single-core pinning, unchanged from its months-proven behavior. */
        if (g_pageflip_mode) {
            unsigned long cpumask = 0x3;
            syscall(SYS_sched_setaffinity, 0, sizeof(cpumask), &cpumask);
        }
        dup2(pfd[0], 0);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        nice(-5);
        /* Built as an array (execvp) rather than the execlp literal it used
         * to be solely so the -sub/-subdelay pair can be conditional. */
        const char *args[64];
        int an = 0;
        args[an++] = "mplayer";
        args[an++] = "-slave";      args[an++] = "-quiet";
        args[an++] = "-nojoystick"; args[an++] = "-noconsolecontrols";
        args[an++] = "-vo";         args[an++] = "fbdev:/dev/fb0";
        args[an++] = "-ao";         args[an++] = "alsa";
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
        args[an++] = "-osdlevel"; args[an++] = "0";
        /* mplayer draws its OSD straight into the physical framebuffer,
         * bypassing the app's own line-doubling — the same reason the
         * subtitle font gets a dedicated 2x atlas for this mode (see
         * subfont2x above) applies here too, confirmed on hardware: the
         * ordinary font read vertically squished once OSD text became
         * visible on the interlaced raster. ui_scaled qualifies for the
         * same reason from the other direction: the video window there is
         * ~720 physical lines, where the 1x glyphs read half-size. */
        args[an++] = "-font";
        args[an++] = (fb->line_double || (fb->ui_scaled && fb->phys_height >= 480))
                         ? "/media/fat/misterfin/font2x/font.desc"
                         : "/media/fat/misterfin/font/font.desc";
        args[an++] = "-framedrop";
        args[an++] = "-autosync"; args[an++] = "30";
        args[an++] = "-cache";    args[an++] = "8192";
        args[an++] = "-cache-min"; args[an++] = "20";
        /* mplayer's own native MPEG-TS demuxer sometimes misses the
         * video PID on Jellyfin's transcoded TS output (only finds
         * audio) — forcing the ffmpeg/libavformat demuxer instead
         * fixes it. Confirmed against a real Jellyfin 10.11 server. */
        args[an++] = "-demuxer"; args[an++] = "lavf";
        /* -sws 0 = fast bilinear instead of the (much costlier)
         * default bicubic scaler — confirmed empirically to matter
         * a lot given there's no NEON path in this build. */
        args[an++] = "-sws"; args[an++] = "0";
               /* Decode is requested small server-side (see profile below)
                * to keep the software H.264 decode cheap; this -vf chain
                * scales it back up to fill /dev/fb0's actual live size
                * (cheap relative to decode) while preserving the source's
                * own aspect ratio and letterboxing/pillarboxing with black
                * bars — scale=W:-1 fits the wider dimension, expand pads
                * the other to WxH, and dsize=WxH is required last: without
                * it mplayer's own internal aspect "prescale" logic
                * recomputes and overrides the final display size (confirmed
                * on hardware — omitting dsize gave a distorted stretch or a
                * wrong-sized output depending on the rest of the chain).
                * Built from fb->width/height above — was hardcoded to
                * 640x288 (the PAL framebuffer size), which caused green
                * corruption artifacts once tested against a 240-tall NTSC
                * framebuffer. */
        args[an++] = "-vf";       args[an++] = vf_arg;
        /* vo_fbdev anchors the dsize'd frame at (0,0) unless -geometry
         * says otherwise — invisible on every CRT setup, where the canvas
         * IS exactly dsize-sized, but on an HDMI canvas (ui_scaled — see
         * fb.h) the video window sat top-left while the scaled UI drew
         * centered. geom_arg (set alongside vw/vh above) pins the window
         * to the UI's own 4:3 box; gated on ui_scaled so no CRT invocation
         * ever gains a new argument. */
        if (have_geom) {
            args[an++] = "-geometry"; args[an++] = geom_arg;
        }
        args[an++] = "-lavdopts"; args[an++] = "threads=2:fast";
               /* volume=-3: a few dB of headroom before the s16 conversion.
                * Loudness-war-era masters sit at 0dBFS and lossy decode
                * (the stream's audio is always mp3, see jf_stream_url)
                * overshoots full scale on exactly that material, so with no
                * headroom the peaks hard-clip somewhere between the s16
                * conversion here and the FPGA audio path — audible as
                * "breaking up" on loud passages on the MiSTer while the
                * same file sounds fine on a desktop (float pipelines with
                * the mixer below max never hit the ceiling). -3dB is barely
                * a level change but absorbs typical lossy overshoot.
                * jf_stream_url pins the stream's audio to 48kHz to match
                * the ALSA sink — see the comment there. */
        args[an++] = "-af";       args[an++] = "volume=-3,format=s16le";
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
        /* The sanitized .srt we write is UTF-8 (jf_text_to_display now
         * keeps Latin-1 accents rather than folding them). -utf8 tells
         * mplayer to decode it as UTF-8 and look each code point up in
         * the subtitle font, which now carries U+00A0-U+00FF glyphs
         * (see gen_subfont.py) — so accented subtitles render instead
         * of dropping the accented characters. */
        args[an++] = "-utf8";
        /* See cmdline_sub above (issue #12): the (re)start's subtitle
         * selection must be a startup option, not an early slave command. */
        if (cmdline_sub) {
            args[an++] = "-sub";      args[an++] = SUB_LOCAL_PATH;
            args[an++] = "-subdelay"; args[an++] = subdelay_arg;
        }
        /* mplayer lays glyphs out in PHYSICAL lines: on the interlaced
         * full-frame (line_double) buffers the 13px progressive subfont
         * shows up half-size and squashed, so those modes get a dedicated
         * 24px atlas — an exact 3x of the 8x8 glyphs, so every stroke stays
         * the same thickness (13px's fractional 1.625x scale mixes 1px and
         * 2px strokes), visually matching 12px at 288 lines. ui_scaled's
         * ~720-line video window has the same physical-line-count reason
         * as the -font choice above. */
        args[an++] = "-subfont";
        args[an++] = (fb->line_double || (fb->ui_scaled && fb->phys_height >= 480))
                         ? "/media/fat/misterfin/subfont2x/font.desc"
                         : "/media/fat/misterfin/subfont/font.desc";
        args[an++] = "-subwidth"; args[an++] = "90";
        args[an++] = "-subpos";   args[an++] = "92";
        /* Audio consistently trails video by a small fixed amount
         * (reported by the user across titles — not load-dependent,
         * so not the same issue fb_wait_vsync() fixed). First guess
         * was -0.10 — confirmed on hardware to make the gap BIGGER,
         * so the sign was backwards for this mplayer build. Flipped
         * to positive and roughly doubled the magnitude (since the
         * wrong-signed -0.10 visibly widened the gap by about that
         * much) — needs live tuning against further hardware
         * feedback, sign/magnitude both still empirical. */
        args[an++] = "-delay"; args[an++] = delay_arg;
        args[an++] = url;
        args[an]   = NULL;
        execvp(MPLAYER, (char *const *)args);
        _exit(1);
    }

    close(pfd[0]);
    g_cmd_fd = pfd[1];

    /* mplayer is connecting + filling its cache in the background at this
     * point and hasn't touched /dev/fb0 yet — safe window to show the
     * loading spinner without racing its own frame writes. Under ui_scaled
     * the video window can be smaller than the UI's 4:3 box, so blank the
     * SCREEN first (clear + flip, not just the back buffer: spinner_show's
     * own first act is fb_sync_back, which would pull the still-displayed
     * UI right back over a back-buffer-only clear — confirmed on hardware
     * as the UI lingering around the video window). Everywhere else the
     * old behavior (last screen stays visible behind the spinner) is
     * deliberate — see the no-clear comment at the top of this function. */
    if (fb->ui_scaled) { fb_clear(fb); fb_flip(fb); }
    spinner_show(fb, 2.0);

    /* Nothing subtitle-related to send here: a client-rendered selection
     * rides the command line (see cmdline_sub above — slave commands sent
     * this early get their sub_delay wiped by playback init, issue #12),
     * and a burned-in one is baked into the url. */
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
    g_np_title_shown_until = now_sec() + 5.0;   /* flash the new title in immersive mode */

    unlink(AF_EXPORT_PATH);   /* don't let the visualizer read a stale previous track's data */

    jf_make_play_session_id(g_play_session_id, sizeof(g_play_session_id));
    char url[700];
    jf_audio_stream_url(&g_cfg, it->id, g_play_session_id, url, sizeof(url));
    stream_via_curl_if_https(url, sizeof(url));

    g_play_offset     = 0.0;
    g_play_start_wall = now_sec();
    g_paused          = 0;
    g_last_progress_report = now_sec();
    jf_report_start(&g_cfg, it->id, g_play_session_id, 0, "DirectStream");

    int pfd[2];
    pipe(pfd);

    g_player_pid = fork();
    if (g_player_pid == 0) {
        setpgid(0, 0);
        /* Same both-cores affinity as the video player, same page-flip-only
         * gating — see play()'s comment. */
        if (g_pageflip_mode) {
            unsigned long cpumask = 0x3;
            syscall(SYS_sched_setaffinity, 0, sizeof(cpumask), &cpumask);
        }
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
                * directly with a real FLAC track on hardware). Kept FIRST
                * in the chain so the visualizer sees the same signal it
                * always has, unaffected by the two filters after it.
                *
                * volume=-3: same headroom as the video player — see
                * play()'s comment.
                *
                * lavcresample=48000: music plays the ORIGINAL file
                * (static=true, no transcode), which is usually 44.1kHz CD
                * material — but the MiSTer's ALSA default device force-
                * resamples everything to 48kHz through ALSA's plain linear-
                * interpolation rate plugin (no quality converter installed;
                * see /etc/asound.conf's rate->file->/dev/MrAudio chain on
                * the device). Linear resampling adds aliasing distortion
                * that scales with signal level — audible as loud music
                * slowly starting to crackle. Resampling here instead, with
                * mplayer's much better polyphase resampler, hands ALSA a
                * 48kHz stream so its own resampler drops out of the path.
                * Placed after volume so the resampler gets the same
                * headroom. */
               "-af", "export=" AF_EXPORT_PATH ":512,volume=-3,lavcresample=48000,format=s16le",
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


static void draw_now_playing(FBDev *fb, JfItem *it, double pos)
{
    fb_clear(fb);

    /* Live PCM for the audio-reactive effect (Nebula) and the VU meters
     * below — read once, up here, so the background pass can use it too.
     * Not read while paused: decode has stopped, so the export file would
     * just return stale pre-pause samples (a frozen meter, per user
     * feedback) instead of settling to empty. */
    int16_t af_buf[4096];
    int af_n = g_paused ? 0 : read_af_samples(af_buf, 4096);

    int nebula = (g_now_playing_bg == NOW_PLAYING_BG_NEBULA);
    int spin_mode = (g_now_playing_bg == NOW_PLAYING_BG_SPIN);
    int tunnel_mode = (g_now_playing_bg == NOW_PLAYING_BG_TUNNEL);
    /* Nebula, Toasty Squadron, Now Spinning, and Tunnel all use the
     * immersive layout — just an overlay (cover + text) over the effect
     * plus the bottom hint bar (Toasty's sprites still fly over the top,
     * see draw_toasty_fg below). */
    int immersive = nebula || spin_mode || tunnel_mode || (g_now_playing_bg == NOW_PLAYING_BG_TOASTY);

    if      (g_now_playing_bg == 1) draw_rain(fb);
    else if (nebula)                 draw_nebula(fb, af_buf, af_n);
    else if (g_now_playing_bg == NOW_PLAYING_BG_TOASTY) { draw_toasty_bg(fb); draw_now_playing_gradient(fb); }
    else if (spin_mode)             {}   /* its own background (the EQ bar) is drawn below, alongside the disc — needs the layout math there */
    else if (tunnel_mode)            draw_tunnel(fb, af_buf, af_n);
    else                            draw_starfield(fb);

    /* The brief "which background" label on SELECT is shown in every mode
     * (that's how the cycle stays legible); the clock and PAUSED marker are
     * hidden in the immersive modes, which keep only the cover + hints. */
    if (now_sec() < g_now_playing_bg_shown_until)
        draw_text(fb, SAFE_X, SAFE_Y, now_playing_mode_name(g_now_playing_bg), 1, COL_HINT);
    /* In the immersive modes the title is otherwise hidden, so flash it at the
     * top for a few seconds on each track change (see g_np_title_shown_until,
     * set in play_audio). Nudged down a line if the bg-name label happens to
     * be up at the same moment so the two don't overlap. Tunnel excluded:
     * it shows the title permanently in its own corner overlay below, so
     * this transient flash would just be redundant clutter there. */
    if (immersive && !tunnel_mode && now_sec() < g_np_title_shown_until) {
        int ty0 = (now_sec() < g_now_playing_bg_shown_until) ? SAFE_Y + 12 : SAFE_Y;
        char tflash[300];
        snprintf(tflash, sizeof(tflash), "%s", it->name);
        truncate_to_width(fb, tflash, 1, fb->width - 2 * SAFE_X);
        draw_text(fb, SAFE_X, ty0, tflash, 1, COL_TITLE);
    }
    if (!immersive) {
        draw_clock(fb);
        if (g_paused)
            draw_text(fb, SAFE_X, SAFE_Y + 10, "PAUSED", 1, COL_RESUME);
    }

    if (tunnel_mode) {
        /* Tunnel's own corner overlay (inherited from the retired VizGif
         * layout, per user request): title/artist small in the top-left,
         * the clock big (scale 2 — twice the usual header clock) in the
         * top-right. The big centered cover still comes from the shared
         * immersive block below — this only ADDS the corners. */

        /* Same top edge as the clock on the opposite corner — except for
         * the brief window right after switching modes, where the "which
         * background" name label occupies this exact row; bumped down out
         * of its way for that window, same pattern the other immersive
         * modes use just above, and back up the instant the label clears. */
        int ty0 = (now_sec() < g_now_playing_bg_shown_until) ? SAFE_Y + 12 : SAFE_Y;
        char tflash[300];
        snprintf(tflash, sizeof(tflash), "%s", it->name);
        truncate_to_width(fb, tflash, 1, fb->width - 2 * SAFE_X);
        draw_text(fb, SAFE_X, ty0, tflash, 1, COL_TITLE);
        if (it->artist[0]) {
            char abuf[300];
            snprintf(abuf, sizeof(abuf), "%s", it->artist);
            truncate_to_width(fb, abuf, 1, fb->width - 2 * SAFE_X);
            draw_text(fb, SAFE_X, ty0 + 10, abuf, 1, COL_ITEM);
        }

        /* draw_clock() is the shared scale-1 header clock — this mode
         * wants it double-size per user request, so it draws its own. */
        time_t now_t = time(NULL);
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        char clock_buf[8];
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", now_tm.tm_hour, now_tm.tm_min);
        draw_text(fb, fb->width - SAFE_X - text_width(fb, clock_buf, 2), SAFE_Y,
                  clock_buf, 2, COL_HINT);
    }

    if (immersive) {
        /* Immersive mode: enlarged cover centered in the screen over the
         * effect, and nothing else but the shared hint bar below. Seeking
         * still works (L/R input untouched) — only its timeline readout is
         * hidden. */
        int top    = (fb->height == 288) ? SAFE_Y : 2;
        int bottom = fb->height - 8 - SAFE_Y_BOT - 6;

        if (spin_mode) {
            /* Now Spinning's own background: a full-width graphic-EQ bar, reserved
             * as a strip at the bottom of the immersive area (above the
             * hint line) — the disc above shrinks to make room for it,
             * same "top..bottom" box the cover math below already uses. */
            int eq_h = (fb->height == 288) ? 40 : 34;
            int eq_top = bottom - eq_h;
            draw_now_playing_eq(fb, af_buf, af_n, SAFE_X, eq_top, fb->width - 2 * SAFE_X, eq_h);
            bottom = eq_top - 6;
        }

        if (g_nowplaying_cover_px) {
            int box_h  = bottom - top;
            int box_w  = (fb->height == 288) ? box_h : (int)(box_h * par_correction(fb) + 0.5);
            /* Cap at the square box PAL's own math lands on (246 px at 288
             * lines). Without it, the par-corrected box on shorter buffers
             * is never width-constrained, so a square cover fills box_h's
             * full physical height (~88% of the screen at NTSC's 240 lines)
             * instead of PAL's ~51%. Both buffers are 640 px wide, so equal
             * pixel width == equal physical on-screen size; the aspect fit
             * inside blit_fit_centered brings the height down to match.
             * A no-op at 288 lines (box_w is already exactly 246 there). */
            if (box_w > 246) box_w = 246;
            if (spin_mode) {
                /* Gentle spin, eased to a stop while paused and back up to
                 * speed on resume — see now_spinning_disc_angle(). */
                double blur_span;
                double angle = now_spinning_disc_angle(g_paused, &blur_span);
                blur_span *= 0.7;   /* the physically-exact span read a bit strong — toned down */
                /* A bit smaller than the other immersive covers' full box,
                 * per user feedback — still centered in the same area. */
                int disc_h = (int)(box_h * 0.8 + 0.5);
                draw_now_playing_cd(fb, g_nowplaying_cover_px, g_nowplaying_cover_w, g_nowplaying_cover_h,
                                    fb->width / 2, (top + bottom) / 2, disc_h, angle, blur_span);
            } else {
                blit_fit_centered(fb, g_nowplaying_cover_px, g_nowplaying_cover_w, g_nowplaying_cover_h,
                                   fb->width / 2, (top + bottom) / 2, box_w, box_h, 255);
            }
        }
        goto np_hint;
    }

    /* cover_max is solved from the space actually available down to the
     * hint bar, then capped at 165 (the original PAL-tuned size) so this is
     * a no-op at 288 active lines. The fixed 63 below is every OTHER
     * element between the cover and the hint bar that doesn't shrink with
     * resolution (title/subtitle/timeline gaps + both VU meters — see the
     * ty += ... chain below, UNCHANGED from before — deliberately NOT
     * touched here, only cover_top/the safety margin move). On non-PAL
     * heights cover_top is raised close to the top edge (cover grows
     * upward, not downward) so a bigger cover doesn't push that chain any
     * later than it already sits — the safety margin is UNCHANGED (still
     * 6, same as PAL) specifically so the bottom edge (cover_top+cover_max)
     * lands at the exact same place regardless of cover_top, leaving
     * everything below the cover untouched. */
    int cover_top = (fb->height == 288) ? SAFE_Y : 2;
    int cover_max = (fb->height - 8 - SAFE_Y_BOT) - cover_top - 63 - 6;
    if (cover_max > 165) cover_max = 165;
    if (cover_max < 80) cover_max = 80;
    int cy = cover_top + cover_max / 2;

    /* No placeholder box when there's genuinely no cover (track has no
     * embedded art and no album fallback either, see JfItem.image_tag) —
     * per user request, empty space reads better than a gray rectangle
     * that looks like a broken image. */
    if (g_nowplaying_cover_px) {
        /* blit_fit_centered's par_correction() math assumes max_w/max_h
         * describe a FRAMEBUFFER-PIXEL box, but album covers are
         * physically square — passing cover_max for both (a literal
         * square box) makes a square cover's PAR-corrected width come out
         * roughly 2x its height at NTSC (1.67x at PAL), which the function
         * then has to clamp back down, more than halving what actually
         * gets drawn regardless of how big cover_max itself is. Widening
         * max_w by par_correction() describes the box as physically
         * square instead, so the clamp never triggers and the cover
         * actually reaches cover_max. PAL kept passing cover_max/cover_max
         * unchanged — this bug technically exists there too (milder,
         * since its par_correction is smaller) but nobody's ever reported
         * it and PAL isn't being touched here. */
        /* Now that the clamp above no longer silently shrinks it, the full
         * cover_max came out visually too big on NTSC — 75% of it (still
         * centered within the same reserved cover_top..cover_max area, so
         * nothing below shifts) reads better. PAL untouched as above. */
        int cover_disp_h = (fb->height == 288) ? cover_max : (int)(cover_max * 0.75 + 0.5);
        int cover_max_w = (fb->height == 288)
            ? cover_disp_h
            : (int)(cover_disp_h * par_correction(fb) + 0.5);
        blit_fit_centered(fb, g_nowplaying_cover_px, g_nowplaying_cover_w, g_nowplaying_cover_h,
                           fb->width / 2, cy, cover_max_w, cover_disp_h, 255);
    }

    int ty = cover_top + cover_max - 3;
    draw_text(fb, (fb->width - text_width(fb, it->name, 1)) / 2, ty, it->name, 1, COL_TITLE);
    ty += 10;

    char sub[300] = {0};
    if (it->artist[0] && it->album[0])
        snprintf(sub, sizeof(sub), "%s - %s", it->artist, it->album);
    else if (it->artist[0])
        snprintf(sub, sizeof(sub), "%s", it->artist);
    else if (it->album[0])
        snprintf(sub, sizeof(sub), "%s", it->album);
    if (sub[0]) {
        truncate_to_width(fb, sub, 1, fb->width - 2 * SAFE_X);
        draw_text(fb, (fb->width - text_width(fb, sub, 1)) / 2, ty, sub, 1, COL_ITEM);
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

np_hint:
    /* Hint line stays at the SAME height as every other screen's hint row
     * (fb->height - 8 - SAFE_Y_BOT) — everything above it got tightened/
     * moved up instead, per user feedback that this must stay consistent. */
    {
    const char *hint = g_paused ? "B:resume  L/R:seek  U/D:prev/next  SELECT:bg  A:stop"
                                 : "B:pause  L/R:seek  U/D:prev/next  SELECT:bg  A:stop";
    draw_text(fb, (fb->width - text_width(fb, hint, 1)) / 2,
              fb->height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);
    }

    /* Mega-tier Toasty sprites fly over everything above — see
     * draw_toasty_fg()'s own comment. */
    if (g_now_playing_bg == NOW_PLAYING_BG_TOASTY) draw_toasty_fg(fb);

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
    fb.phys_height = fb.height;   /* no line doubling in the fabricated fb */
    fb.mmap_size = (size_t)fb.stride * fb.height;
    fb.mem  = calloc(1, fb.mmap_size);
    fb.back = calloc(1, fb.mmap_size);
    if (!fb.mem || !fb.back) { fprintf(stderr, "alloc failed\n"); return 1; }

    if (!jf_config_load(&g_cfg)) { fprintf(stderr, "jellyfin.conf not found\n"); return 1; }
    if (jf_resolve_user_id(&g_cfg) != 1) { fprintf(stderr, "user resolve failed\n"); return 1; }
    grid_init(&g_cfg);

    g_root_list_mode = list_mode;
    push_frame(FRAME_VIEWS, "MiSTerFin", NULL, NULL, NULL, NULL);
    if (sel >= 0 && sel < g_item_count) g_sel = sel;

    draw_browse(&fb);

    FILE *f = fopen("/tmp/preview.raw", "wb");
    if (f) { fwrite(fb.mem, 1, fb.mmap_size, f); fclose(f); }
    printf("%d %d %d\n", fb.width, fb.height, fb.stride);
    return 0;
}

/* Dev tool for the track picker's layout. The picker only ever appears over
 * live playback, which needs a real framebuffer for mplayer to write into and
 * so can't be reached off-hardware at all — without this there is no way to
 * look at it short of deploying to a MiSTer and starting a film. Fetches one
 * real item's streams from the server, draws the menu over a blank frame, and
 * dumps it for tools/raw_to_png.py.
 *
 * Honours MISTERFIN_FB for geometry (see fb.c), so the same item can be
 * checked at PAL's 288 lines and NTSC's 240 — the box is sized from its
 * content and the shorter frame is where it would overflow first. */
/* Hidden dev tool for the Now Spinning now-playing screen (spinning-disc cover +
 * graphic EQ bar) — no server, no mplayer needed: a synthetic four-quadrant
 * cover (with a white spoke so rotation is unambiguous) stands in for real
 * art, and a fabricated three-tone PCM buffer (bass/mid/treble sines)
 * stands in for read_af_samples so the EQ's per-band bandpass filters have
 * something frequency-varied to visibly react to. t_offset shifts the
 * disc's rotation angle by that many "seconds" (draw_now_playing_cd is
 * driven by now_sec(), so this sleeps for real rather than faking a
 * clock). Dumps via MISTERFIN_FRAME_OUT like every other headless tool. */
static int run_preview_now_spinning(double t_offset)
{
    FBDev fb;
    if (fb_open(&fb, "/dev/fb0") < 0) { fprintf(stderr, "no framebuffer\n"); return 1; }
    SAFE_Y = (int)(SAFE_X / par_correction(&fb) + 0.5);

    int cw = 160, ch = 160;
    uint8_t *cover = malloc((size_t)cw * ch * 4);
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            uint8_t *p = cover + ((size_t)y * cw + x) * 4;
            int qx = x < cw / 2, qy = y < ch / 2;
            p[0] = qx && qy ? 220 : (qx && !qy ? 40  : (!qx && qy ? 40  : 220));
            p[1] = qx && qy ? 40  : (qx && !qy ? 220 : (!qx && qy ? 40  : 220));
            p[2] = qx && qy ? 40  : (qx && !qy ? 40  : (!qx && qy ? 220 : 40));
            p[3] = 255;
            double u = (x - cw / 2.0 + 0.5) / (cw / 2.0);
            double v = (y - ch / 2.0 + 0.5) / (ch / 2.0);
            if (fabs(u) < 0.05 && v < 0) { p[0] = p[1] = p[2] = 255; }
        }
    }

    if (t_offset > 0) usleep((useconds_t)(t_offset * 1e6));

    fb_clear(&fb);

    int top    = (fb.height == 288) ? SAFE_Y : 2;
    int bottom = fb.height - 8 - SAFE_Y_BOT - 6;
    int eq_h   = (fb.height == 288) ? 40 : 34;
    int eq_top = bottom - eq_h;

    int16_t buf[2048];
    int half = 1024;
    double f1 = getenv("SPIN_F1") ? atof(getenv("SPIN_F1")) : 150.0;
    double f2 = getenv("SPIN_F2") ? atof(getenv("SPIN_F2")) : 1000.0;
    double f3 = getenv("SPIN_F3") ? atof(getenv("SPIN_F3")) : 5000.0;
    int reps = getenv("SPIN_REPS") ? atoi(getenv("SPIN_REPS")) : 40;
    double phase = 0.0;
    for (int r = 0; r < reps; r++) {
        for (int i = 0; i < half; i++) {
            double t = phase + i / 44100.0;
            double s = 0.5  * sin(2.0 * M_PI * f1 * t)
                     + 0.3  * sin(2.0 * M_PI * f2 * t)
                     + 0.15 * sin(2.0 * M_PI * f3 * t);
            int16_t v = (int16_t)(s * 20000.0);
            buf[i] = v; buf[half + i] = v;
        }
        phase += half / 44100.0;
        /* Clear before each rep, same as a real frame — the filters' state
         * (eq_x1/x2/y1/y2/eq_level) persists across calls same as always,
         * but without this the dim "unlit" alpha=50 draws blend against
         * whatever a NOISY EARLY (pre-settled) rep already painted fully
         * bright, and same-hue-over-same-hue alpha blending never erases
         * that — a test-harness-only artifact, since real playback always
         * clears before every redraw. */
        fb_clear(&fb);
        draw_now_playing_eq(&fb, buf, half * 2, SAFE_X, eq_top, fb.width - 2 * SAFE_X, eq_h);
    }

    int cd_bottom = eq_top - 6;
    double angle = fmod(now_sec() * 22.5, 2.0 * M_PI);
    draw_now_playing_cd(&fb, cover, cw, ch, fb.width / 2, (top + cd_bottom) / 2, cd_bottom - top, angle, 0.4);

    const char *hint = "B:pause  L/R:seek  U/D:prev/next  SELECT:bg  A:stop";
    draw_text(&fb, (fb.width - text_width(&fb, hint, 1)) / 2, fb.height - 8 - SAFE_Y_BOT, hint, 1, COL_HINT);

    fb_flip(&fb);
    free(cover);
    return 0;
}

/* Hidden dev tool for the Tunnel now-playing mode — runs draw_tunnel in a
 * real-time loop (it's dt-based, so faking time isn't an option) at a
 * settable constant "energy" level, for tuning speed/wobble/colour without
 * a live playback round-trip. Dumps only the final frame like every other
 * preview tool; TUNNEL_SECS/TUNNEL_ENERGY are read from the environment so
 * a tuning pass doesn't need a rebuild between tries. */
static int run_preview_tunnel(void)
{
    FBDev fb;
    if (fb_open(&fb, "/dev/fb0") < 0) { fprintf(stderr, "no framebuffer\n"); return 1; }

    double secs   = getenv("TUNNEL_SECS")   ? atof(getenv("TUNNEL_SECS"))   : 3.0;
    double energy = getenv("TUNNEL_ENERGY") ? atof(getenv("TUNNEL_ENERGY")) : 0.0;
    double bpm    = getenv("TUNNEL_BPM")    ? atof(getenv("TUNNEL_BPM"))    : 0.0;
    int16_t buf[256];

    double start = now_sec();
    int frames = 0;
    while (now_sec() - start < secs) {
        /* Optional TUNNEL_BPM pulses the level like a kick drum would —
         * a flat constant level can never trip the transient detector,
         * so without this the beat-lit panels are untestable headless. */
        double lvl = energy;
        if (bpm > 0.0) {
            double ph = fmod((now_sec() - start) * bpm / 60.0, 1.0);
            lvl = energy * (0.25 + 0.75 * exp(-ph * 6.0));
        }
        int16_t s = (int16_t)(lvl * 32767.0);
        for (int i = 0; i < 256; i++) buf[i] = s;

        fb_clear(&fb);
        draw_tunnel(&fb, buf, 256);
        fb_flip(&fb);
        frames++;
        if (fb.headless) usleep(20000);
    }
    fprintf(stderr, "tunnel: %d frames in %.2fs = %.1f fps\n", frames, now_sec() - start, frames / (now_sec() - start));
    return 0;
}

static int run_preview_submenu(const char *item_id, int tab)
{
    FBDev fb;
    if (fb_open(&fb, "/dev/fb0") < 0) { fprintf(stderr, "no framebuffer\n"); return 1; }
    SAFE_Y = (int)(SAFE_X / par_correction(&fb) + 0.5);

    if (!jf_config_load(&g_cfg))          { fprintf(stderr, "jellyfin.conf not found\n"); return 1; }
    if (jf_resolve_user_id(&g_cfg) != 1)  { fprintf(stderr, "user resolve failed\n"); return 1; }
    if (!jf_get_item_details(&g_cfg, item_id, &g_info_item)) {
        fprintf(stderr, "could not fetch item %s\n", item_id);
        return 1;
    }

    fprintf(stderr, "%s: %d audio track(s), %d subtitle track(s)\n",
            g_info_item.name, g_info_item.audio_count, g_info_item.sub_count);

    g_current_audio_index = default_audio_index();
    g_current_sub_index   = g_info_item.sub_count > 0 ? g_info_item.subs[0].index : -1;
    g_submenu_visible     = 1;
    g_submenu_audio_sel   = 0;
    g_submenu_sub_sel     = 0;

    fb_clear(&fb);
    memcpy(fb.mem, fb.back, (size_t)fb.stride * fb.height);
    submenu_bg_capture(&fb);

    /* Draw the OTHER tab first and only then the requested one, so what gets
     * dumped is a frame that has been switched to rather than opened on. The
     * two tabs are different sizes, so this is the path where the taller
     * box's leftovers used to survive around the shorter one — rendering a
     * single tab in isolation would never show it. */
    g_submenu_tab = (tab == SUBMENU_TAB_AUDIO) ? SUBMENU_TAB_SUBS : SUBMENU_TAB_AUDIO;
    draw_submenu(&fb);
    g_submenu_tab = tab;
    draw_submenu(&fb);

    printf("%d %d %d\n", fb.width, fb.height, fb.stride);
    fb_close(&fb);
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

/* ── startup resolve (backgrounded so the black screen at launch can show
 * the blinking corner spinner instead of just sitting there) ──────────────
 * jf_config_load + jf_resolve_user_id + the initial library listing
 * (push_frame's fetch_frame call) are a few sequential blocking network
 * round-trips — on a slow/distant server this was the whole cause of the
 * occasional 5-6s black-screen pause at launch, not SD card reads. Running
 * them on a thread and blinking the spinner on the main thread in the
 * meantime turns that dead pause into visible feedback; when the server's
 * fast this thread finishes before the first spinner flip even happens, so
 * the screen just goes black-to-browse same as before — no flash. */
#define STARTUP_PENDING -2
#define STARTUP_CONFIG_MISSING -3
#define STARTUP_NEED_QUICK_CONNECT -4
static pthread_mutex_t g_startup_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_startup_result = STARTUP_PENDING;

/* Loads the library listing once a credential is known good. Shared by the
 * normal startup path and by the Quick Connect one, which reaches this point
 * minutes later and from a different thread. */
static void startup_enter_browse(void)
{
    push_frame(FRAME_VIEWS, "MiSTerFin", NULL, NULL, NULL, NULL);
}

static void *startup_resolve_thread(void *arg)
{
    (void)arg;
    int result;

    if (!jf_config_load(&g_cfg)) {
        pthread_mutex_lock(&g_startup_mutex);
        g_startup_result = STARTUP_CONFIG_MISSING;
        pthread_mutex_unlock(&g_startup_mutex);
        return NULL;
    }

    /* Before any request: this goes in every Authorization header, and must
     * be identical between authenticating and using the resulting token. */
    jf_device_id_init(&g_cfg);

    jf_log_init(&g_cfg);   /* no-op unless "DEBUGLOG" is in jellyfin.conf */

    if (jf_token_load(&g_cfg) && jf_credential_works(&g_cfg)) {
        /* A previously earned Quick Connect token, still accepted. Checked
         * before the config's API key so that re-authenticating once sticks
         * even if a stale key is left in the file. */
        result = 1;
        startup_enter_browse();
    } else if (g_cfg.api_key[0] && g_cfg.username[0]) {
        /* Classic path: an admin-issued API key plus a username to resolve.
         * jf_token_load may have overwritten the credential with a dead
         * token, so put the key back first. */
        strncpy(g_cfg.token, g_cfg.api_key, sizeof(g_cfg.token) - 1);
        g_cfg.user_id[0] = '\0';
        result = jf_resolve_user_id(&g_cfg);
        if (result == 1) startup_enter_browse();
    } else {
        /* No usable credential — a saved token that no longer works ends up
         * here too, which is what makes a revoked or expired login recover by
         * itself instead of showing an error nobody can act on. */
        jf_token_clear();
        g_cfg.token[0] = '\0';
        g_cfg.user_id[0] = '\0';
        result = STARTUP_NEED_QUICK_CONNECT;
    }

    pthread_mutex_lock(&g_startup_mutex);
    g_startup_result = result;
    pthread_mutex_unlock(&g_startup_mutex);
    return NULL;
}

/* A handful of MiSTer.ini keys that determine what MiSTerFin's framebuffer
 * output actually looks like on the far end of the cable (see
 * docs/DISPLAY_COMPATIBILITY.md) — logging these turns "picture is wrong on
 * my setup" into something diagnosable from the log alone, rather than
 * needing someone to SSH in and ask for MiSTer.ini by hand. A value inside
 * [Menu] is what actually governs a Script's own framebuffer geometry
 * (confirmed on hardware — see that doc's video_mode section), so the
 * section a match was found under is reported alongside it rather than
 * just assuming top-level. Whitelisted by key name, not a full-file dump —
 * same "narrow by construction" rule as the request log's query-string cut. */
static void log_display_settings(void)
{
    static const char *const keys[] = {
        "ypbpr", "composite_sync", "forced_scandoubler", "vga_scaler",
        "direct_video", "vsync_adjust", "video_mode", "video_mode_ntsc",
        "video_mode_pal", NULL
    };
    FILE *f = fopen("/media/fat/MiSTer.ini", "r");
    if (!f) { jf_log_line("MiSTer.ini: not found"); return; }

    char section[32] = "";
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                size_t n = (size_t)(end - p - 1);
                if (n >= sizeof(section)) n = sizeof(section) - 1;
                memcpy(section, p + 1, n);
                section[n] = '\0';
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        size_t klen = (size_t)(eq - p);
        while (klen > 0 && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) klen--;
        for (int i = 0; keys[i]; i++) {
            if (strlen(keys[i]) != klen || strncasecmp(p, keys[i], klen)) continue;
            char *val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            char *semi = strchr(val, ';');   /* inline comment */
            if (semi) *semi = '\0';
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' ||
                                 val[vlen - 1] == ' '  || val[vlen - 1] == '\t'))
                val[--vlen] = '\0';
            jf_log_line("MiSTer.ini [%s] %.*s=%s", section[0] ? section : "top",
                        (int)klen, p, val);
            break;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--capture-about") == 0)
        return run_capture_about(argc > 2 ? atoi(argv[2]) : 60);
    if (argc > 1 && strcmp(argv[1], "--preview-browse") == 0)
        return run_preview_browse(argc > 2 ? atoi(argv[2]) : -1,
                                   argc > 3 && strcmp(argv[3], "list") == 0);
    if (argc > 1 && strcmp(argv[1], "--preview-submenu") == 0)
        return run_preview_submenu(argc > 2 ? argv[2] : "",
                                    (argc > 3 && strcmp(argv[3], "audio") == 0)   ? SUBMENU_TAB_AUDIO :
                                    (argc > 3 && strcmp(argv[3], "picture") == 0) ? SUBMENU_TAB_PICTURE
                                                                                    : SUBMENU_TAB_SUBS);
    if (argc > 1 && strcmp(argv[1], "--preview-now-spinning") == 0)
        return run_preview_now_spinning(argc > 2 ? atof(argv[2]) : 0.0);
    if (argc > 1 && strcmp(argv[1], "--preview-tunnel") == 0)
        return run_preview_tunnel();

    srand((unsigned)time(NULL));   /* for the About screen's starfield */
    grid_init(&g_cfg);             /* just stores the pointer — config itself loads below */
    /* Before fb_open and the config load on purpose: sfx_init only reads two
     * files and starts a thread, and doing it here means the very first
     * keypress on the setup screen already clicks. Never fatal — see sfx.h. */
    {
        const char *sfx_dir = getenv("MISTERFIN_SFX_DIR");
        sfx_init(sfx_dir && *sfx_dir ? sfx_dir : SFX_DIR);
    }
    start_cache_sweep();

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

    bgm_pause();   /* see bgm_pause's own comment; undone by bgm_resume() below
                     * and, on a crash, by emergency_cleanup(). Deliberately
                     * after the fb_open check above, which returns early on
                     * failure — pausing before that would leave BGM stuck
                     * paused with nothing left running to resume it. */

    g_headless = fb.headless;   /* see g_headless' own comment */
    g_pageflip_mode = fb.interlaced_core;   /* see pageflip_begin()'s comment */
    /* The interlaced menu core builds its own raster (in-core modeline
     * conversion — different active width and porches than the progressive
     * modes the 24px margin was tuned against over months), and on a real
     * CRT it overscans noticeably more: confirmed on hardware that edges
     * of the UI get eaten at the stock margin. Widen the title-safe zone
     * for that mode only; progressive stays exactly as tuned. Kept on
     * line_double (not interlaced_core): a progressive HDMI 480/576 canvas
     * shares the layout, and a slightly wider margin is harmless there. */
    if (fb.line_double) SAFE_X = 36;
    /* SAFE_Y as a plain pixel count made the top/bottom margin look
     * noticeably BIGGER than the left/right margin on real hardware, even
     * though 20 < 24 — because our pixels aren't square. Physically,
     * SAFE_Y rows are worth more screen distance than SAFE_X columns by
     * exactly par_correction()'s factor, so dividing by it here equalizes
     * the four margins in real physical terms instead of raw pixel count.
     * At fb->height=288 this comes out to 24/1.667≈14 (was a flat 20) —
     * confirmed as a real improvement there too, not NTSC-only. */
    SAFE_Y = (int)(SAFE_X / par_correction(&fb) + 0.5);
    fb_flip(&fb);   /* push the cleared back buffer to screen (line-doubles if needed) */

    cursor_hide();
    input_open();
    input_drain();

    fb_set_overlay(banner_overlay);   /* session messages — see banner_overlay */
    unlink(BANNER_FILE);              /* stale handoff from a crashed previous run */

    /* Enable vsync by default — mplayer's patched vo_fbdev checks this file
     * each frame (see VSYNC_FLAG comment above).
     *
     * EXCEPT on the real interlaced core (interlaced_core, not the broader
     * line_double geometry — a progressive HDMI 480/576 canvas scans out
     * normally and takes the ordinary vsync default): that mode is
     * tear-free via hardware page-flip instead (pf_active in vo_fbdev.c's
     * draw_slice() skips this wait entirely, so the flag is normally moot
     * there), and L/R is disabled during playback in that mode for the same
     * reason (see the INP_L/INP_R handling above). Clearing it here is a
     * fallback for the rare case page-flip itself fails to engage (e.g. the
     * MiSTer_fb mmap in pf_init() fails) — measured on hardware before
     * page-flip existed that this per-frame wait doesn't line up with that
     * scanout mode's field timing (A-V drift grew 11s in 45s with the flag
     * on, vs. 0.000 with it off), so leaving it off is the safer default if
     * that fallback path is ever hit. */
    if (!fb.interlaced_core) {
        int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644); if (vf >= 0) close(vf);
    } else {
        unlink(VSYNC_FLAG);   /* clear a stale flag from a previous run */
    }

    pthread_t startup_tid;
    pthread_create(&startup_tid, NULL, startup_resolve_thread, NULL);
    {
        int spinner_frame = 0;
        for (;;) {
            pthread_mutex_lock(&g_startup_mutex);
            int done = (g_startup_result != STARTUP_PENDING);
            pthread_mutex_unlock(&g_startup_mutex);
            if (done) break;
            draw_spinner_frame(&fb, spinner_frame++);
            fb_flip(&fb);
            usleep(SPINNER_BLINK_MS * 1000);
        }
    }
    pthread_join(startup_tid, NULL);

    AppState state;
    pthread_t qc_tid;
    int qc_running = 0;
    int resolved = g_startup_result;
    if (resolved == STARTUP_CONFIG_MISSING) {
        state = STATE_CONFIG_ERROR;
        snprintf(g_setup_reason, sizeof(g_setup_reason), "jellyfin.conf not found or incomplete");
        g_setup_help = SETUP_HELP_CONFIG;
        draw_setup_screen(&fb, g_setup_reason, g_setup_help);
    } else if (resolved == STARTUP_NEED_QUICK_CONNECT) {
        state = STATE_QUICK_CONNECT;
        qc_set_state(QC_STARTING);
        pthread_create(&qc_tid, NULL, quick_connect_thread, NULL);
        qc_running = 1;
        draw_quick_connect(&fb);
    } else if (resolved == 1) {
        state = STATE_BROWSE;
    } else {
        state = STATE_CONFIG_ERROR;
        /* -1 = the request itself failed (server unreachable) — don't
         * blame the config for that, it might be perfectly correct and
         * the server's just down/wrong URL. 0 = server answered but no
         * user matched, which really is a config problem. */
        if (resolved == -1 && server_https_needs_insecure_hint())
            /* An https server we couldn't reach with verification on is most
             * likely a self-signed cert — point at the fix rather than the
             * URL. */
            snprintf(g_setup_reason, sizeof(g_setup_reason),
                     "Can't reach server - if HTTPS self-signed, set INSECURE_TLS");
        else
            snprintf(g_setup_reason, sizeof(g_setup_reason),
                     resolved == -1 ? "Can't connect to server (check server URL)"
                                    : "Username not found on server (check spelling)");
        g_setup_help = (resolved == -1) ? SETUP_HELP_CONNECTION : SETUP_HELP_CONFIG;
        draw_setup_screen(&fb, g_setup_reason, g_setup_help);
    }

    /* One-shot startup diagnostics — see jf_log_line's own comment. Placed
     * here rather than earlier because it wants the just-computed startup
     * outcome (which credential path worked, or exactly why none did), not
     * just the inputs to it. A no-op unless DEBUGLOG is set. */
    jf_log_line("fb: %dx%d (real %dx%d) line_double=%d interlaced_core=%d ui_scaled=%d headless=%d",
                fb.width, fb.height, fb.real_width, fb.real_height,
                fb.line_double, fb.interlaced_core, fb.ui_scaled, fb.headless);
    for (int i = 0; i < input_device_count(); i++)
        jf_log_line("input: %s \"%s\"%s", input_device_node(i), input_device_name(i),
                    input_device_is_virtual(i) ? " [MiSTer virtual]" : "");
    log_display_settings();
    jf_log_line("startup: %s",
                resolved == 1 ? "signed in (saved token or API key)" :
                resolved == STARTUP_NEED_QUICK_CONNECT ? "Quick Connect required" :
                resolved == STARTUP_CONFIG_MISSING ? "jellyfin.conf not found or incomplete" :
                g_setup_reason);

    /* Open the server's command socket (admin messages, dashboard remote
     * control — see session.h). Started for the Quick Connect path too:
     * the thread itself waits for the token to appear. */
    if (resolved == 1 || resolved == STARTUP_NEED_QUICK_CONNECT)
        session_start(&g_cfg);

    /* Enable DDR native-video only when menu_zaparoo.rbf is the active menu
     * core — running the DDR copy loop against a core that never reads it
     * adds bus contention without benefit.
     *
     * Detected by comparing menu.rbf's CONTENT against the installed
     * Zaparoo copy (/media/fat/zaparoo/menu_zaparoo.rbf — the exact file
     * a Zaparoo install script copies into place), so detection survives
     * Zaparoo releasing a new build. The old exact-size check stays as a
     * fallback for a card where the zaparoo/ folder was deleted after
     * install. Size compare gates the byte compare, so the common
     * stock-core case costs one stat().
     *
     * The file checks only reflect what boots by default — a manually
     * loaded core (the standalone interlaced one, most likely) is what's
     * actually running, and no file check can see that. The framebuffer
     * geometry can: the interlaced core always presents a 576/480-line
     * buffer (fb.line_double), and the Zaparoo DDR path belongs to a
     * progressive menu core by definition. Without that gate, a user with
     * the Zaparoo core installed as menu.rbf who watches through the
     * interlaced core would burn a per-frame DDR copy + vsync wait on a
     * scanout path the loaded core never reads — precisely in the mode
     * with the least CPU headroom to spare. */
    {
        struct stat mst;
        int zaparoo_active = 0;
        if (stat("/media/fat/menu.rbf", &mst) == 0) {
            struct stat zst;
            if (stat("/media/fat/zaparoo/menu_zaparoo.rbf", &zst) == 0 &&
                zst.st_size == mst.st_size)
                zaparoo_active = files_identical("/media/fat/menu.rbf",
                                                 "/media/fat/zaparoo/menu_zaparoo.rbf");
            if (!zaparoo_active && mst.st_size == 2513448)
                zaparoo_active = 1;   /* legacy build, zaparoo/ folder gone */
        }
        int ddr_engaged = zaparoo_active && !fb.line_double && ddr_init() == 0;
        if (ddr_engaged)
            ddr_set_mode(strcasecmp(g_cfg.tv_mode, "NTSC") == 0 ? 0 : 2);
        jf_log_line("menu core: zaparoo_menu_rbf=%d interlaced_core=%d ddr_engaged=%d",
                    zaparoo_active, fb.interlaced_core, ddr_engaged);
    }

    /* Start GitHub release check in background — result appears on the
     * About screen once it lands. */
    update_check_start();

    /* Silently pre-fetch every library's grid background in the
     * background, so switching to one the user hasn't visited yet this
     * session doesn't show a blank/dim background while it fetches — see
     * grid_prefetch_thread's own comment.
     *
     * Only once there's actually a session to fetch with. It used to start
     * unconditionally, which meant the setup and Quick Connect screens each
     * fired a /UserViews with an empty userId and no credential — harmless
     * against a permissive server, but a guaranteed 401 against a real one,
     * and a confusing entry in the server log for anyone debugging why their
     * client won't connect. */
    if (state == STATE_BROWSE) start_grid_prefetch(&fb);

    int playing = 0;
    int spinner_frame_ctr = 0;
    int about_visible = 0;
    int changelog_visible = 0;   /* what's-new sub-screen over About, see draw_changelog */
    int changelog_scroll = 0;
    double last_about_press = 0.0;
    double screenshot_flash_until = 0.0;
    if (state == STATE_BROWSE) draw_browse(&fb);

    double last_input_rescan = 0.0;
    while (g_running) {
        /* If anything drawn this iteration already called fb_flip(), that
         * blocked on FBIO_WAITFORVSYNC — every usleep(16000) below (the
         * shared tail and the few early-continue screens above it) skips
         * itself when this changed, instead of adding redundant dead time
         * on top of an already-paced frame. Confirmed on hardware (first
         * found on STATE_BROWSE: cutting the home carousel's per-frame CPU
         * cost nearly in half left the achieved redraw rate completely
         * unchanged, because this trailing sleep was absorbing 100% of the
         * savings) then generalized to every screen via g_fb_flip_count
         * (see its own comment) instead of hand-threading a flag through
         * each one. */
        unsigned long flips_before = g_fb_flip_count;
        int inp = input_poll();
        /* Must be called every tick (it drives the repeat timers), but only
         * OR'd in by the screens that want held-to-scroll — see
         * input_repeat()'s own comment for why it isn't just part of inp. */
        int nav_repeat = input_repeat();
        double loop_now = now_sec();

        /* Screenshot: fires once on the transition into "both held", from
         * ANY screen — checked unconditionally here rather than inside the
         * state switch below, which several states short-circuit past via
         * `continue` before reaching a common tail.
         *
         * A human two-finger press is never perfectly simultaneous, and
         * this loop can run faster than the few milliseconds of stagger
         * between them — confirmed on hardware as SELECT's edge alone
         * reaching the video submenu (or About, or the root list toggle)
         * a frame or two before input_select_start_held() ever sees START
         * down too. So neither bit is allowed downstream the instant
         * input_poll() reports it: each is held back for COMBO_WINDOW to
         * give the other a chance to arrive. If the chord completes within
         * that window the screenshot fires and both buffered edges are
         * dropped (never delivered as an ordinary press); otherwise the
         * lone edge is released into `inp`, on whatever later frame the
         * window lapses, exactly as if it had just been pressed then. */
        {
            #define COMBO_WINDOW 0.12
            static int screenshot_combo_was_held = 0;
            static double pending_select_t = -1.0, pending_start_t = -1.0;

            if (inp & INP_SELECT) { pending_select_t = loop_now; inp &= ~INP_SELECT; }
            if (inp & INP_START)  { pending_start_t  = loop_now; inp &= ~INP_START;  }

            int held = input_select_start_held();
            if (held && !screenshot_combo_was_held) {
                screenshot_take(&fb);
                screenshot_flash_until = loop_now + 1.5;
                pending_select_t = pending_start_t = -1.0;
            }
            screenshot_combo_was_held = held;

            if (pending_select_t >= 0.0 && loop_now - pending_select_t > COMBO_WINDOW) {
                inp |= INP_SELECT;
                pending_select_t = -1.0;
            }
            if (pending_start_t >= 0.0 && loop_now - pending_start_t > COMBO_WINDOW) {
                inp |= INP_START;
                pending_start_t = -1.0;
            }
        }

        /* Re-scan /dev/input every few seconds — a wireless pad that idles
         * out and reconnects (confirmed behavior for some 8BitDo/Bluetooth
         * pads) can come back as a brand new event node; without this,
         * whichever fd we opened at startup just goes dead silently and
         * that controller's Start/Select (or any input) stops responding
         * with no visible cause. input_open() only adds devices not
         * already tracked, so this is a cheap no-op when nothing changed. */
        if (loop_now - last_input_rescan > 3.0) {
            last_input_rescan = loop_now;
            input_open();
        }

        /* Server-pushed session events (see session.h). Messages: banner
         * on app-drawn screens, mplayer's own OSD during video (mplayer
         * owns the framebuffer there and would repaint over the banner —
         * the same reasoning as the osdlevel-0 comment in play()).
         * Playstate commands become SYNTHETIC INPUT BITS rather than
         * direct calls — the existing input handlers already own every
         * report/stop/state-transition detail (dashboard Stop runs the
         * exact same path as the user's own stop button), so mapping to
         * them can't drift out of sync with real button behavior. Only
         * injected in the two playing states with no overlay up: anywhere
         * else INP_A/INP_B mean navigation, not transport control. */
        {
            SessionEvent sev;
            while (session_poll(&sev)) {
                if (sev.kind == SESSION_EV_MESSAGE) {
                    char full[BANNER_TEXT_MAX];
                    if (sev.header[0] && sev.text[0])
                        snprintf(full, sizeof(full), "%s: %s", sev.header, sev.text);
                    else
                        snprintf(full, sizeof(full), "%s", sev.text[0] ? sev.text : sev.header);
                    /* Live video: mplayer draws the banner (see BANNER_FILE).
                     * Paused video: mplayer isn't producing frames (the
                     * file would sit unread until resume) but the pause
                     * screen redraws every tick, so the app's own banner
                     * works there — route by pause state. */
                    if (state == STATE_PLAYING && !g_paused)
                        banner_file_write(full);
                    else
                        banner_enqueue(full);
                } else if ((state == STATE_PLAYING || state == STATE_PLAYING_AUDIO) &&
                           !about_visible && !g_submenu_visible) {
                    switch (sev.kind) {
                    case SESSION_EV_PLAY_PAUSE: inp |= INP_A; break;
                    case SESSION_EV_PAUSE:      if (!g_paused) inp |= INP_A; break;
                    case SESSION_EV_UNPAUSE:    if (g_paused)  inp |= INP_A; break;
                    case SESSION_EV_STOP:       inp |= INP_B; break;
                    case SESSION_EV_NEXT:
                        if (state == STATE_PLAYING_AUDIO) inp |= INP_DOWN;
                        break;
                    case SESSION_EV_PREV:
                        if (state == STATE_PLAYING_AUDIO) inp |= INP_UP;
                        break;
                    default: break;
                    }
                }
            }

            /* A message handed to mplayer right as playback ended never got
             * drawn — replay the leftover through the app's own banner. */
            if (state != STATE_PLAYING && access(BANNER_FILE, F_OK) == 0) {
                FILE *bf = fopen(BANNER_FILE, "r");
                if (bf) {
                    char leftover[BANNER_TEXT_MAX];
                    size_t n = fread(leftover, 1, sizeof(leftover) - 1, bf);
                    fclose(bf);
                    while (n > 0 && (leftover[n-1] == '\n' || leftover[n-1] == '\r')) n--;
                    leftover[n] = '\0';
                    if (leftover[0]) banner_enqueue(leftover);
                }
                unlink(BANNER_FILE);
            }
        }

        /* START toggles the About screen — only from the browser, not
         * mid-playback. */
        if (!playing && (inp & INP_START) && (loop_now - last_about_press > 0.3)) {
            last_about_press = loop_now;
            about_visible = !about_visible;
            changelog_visible = 0;
            if (about_visible) draw_about(&fb);
            else redraw_current_screen(&fb, state);
            input_drain();
            continue;
        }
        if (about_visible) {
            /* What's-new sub-screen: shown between "install" on About and
             * the actual install, so the user sees the release notes first.
             * On-screen labels vs INP_* bits follow the app-wide mapping
             * (screen "B" = INP_A, screen "A" = INP_B — same as About's own
             * footer above). */
            if (changelog_visible) {
                if (inp & INP_B) {                    /* screen "A: back" */
                    changelog_visible = 0;
                    draw_about(&fb);
                } else if (inp & INP_A) {             /* screen "B: install update" */
                    update_start_install();
                    changelog_visible = 0;
                    draw_about(&fb);
                } else {
                    int nav = inp | nav_repeat;
                    if (nav & INP_UP)   changelog_scroll--;
                    if (nav & INP_DOWN) changelog_scroll++;
                    int max_scroll = g_cl_count - changelog_rows(&fb);
                    if (max_scroll < 0) max_scroll = 0;
                    if (changelog_scroll < 0) changelog_scroll = 0;
                    if (changelog_scroll > max_scroll) changelog_scroll = max_scroll;
                    /* Same screenshot-flash guard as About's redraw below. */
                    if (loop_now >= screenshot_flash_until)
                        draw_changelog(&fb, changelog_scroll);
                }
                if (g_fb_flip_count == flips_before) usleep(16000);
                continue;
            }
            if (inp & INP_B) {
                about_visible = 0;
                redraw_current_screen(&fb, state);
            } else {
                if (inp & INP_A) {
                    UpdateState  us;
                    InstallState is;
                    update_get_state(&us, &is, NULL, 0);
                    /* Only when the footer is actually offering "B: install"
                     * — mid-download/done/failed the press means nothing,
                     * same as before. */
                    if (us == UPD_AVAILABLE && is == INST_IDLE) {
                        changelog_prepare(&fb);
                        changelog_scroll = 0;
                        changelog_visible = 1;
                        draw_changelog(&fb, 0);
                        input_drain();
                        if (g_fb_flip_count == flips_before) usleep(16000);
                        continue;
                    }
                }
                /* Redraw every frame to pick up update state — except right
                 * after a screenshot taken while About was already open,
                 * where that same unconditional redraw would otherwise
                 * overwrite screenshot_take()'s "Screenshot saved" flash
                 * before it was ever visible. */
                if (loop_now >= screenshot_flash_until) draw_about(&fb);
            }
            if (g_fb_flip_count == flips_before) usleep(16000);
            continue;
        }

        if (g_submenu_visible) {
            submenu_handle_input(&fb, inp, nav_repeat, loop_now);
            continue;
        }

        switch (state) {

        case STATE_CONFIG_ERROR:
            if (inp & (INP_A | INP_B)) { g_running = 0; break; }
            draw_setup_screen(&fb, g_setup_reason, g_setup_help);   /* redrawn every frame for the starfield */
            break;

        case STATE_QUICK_CONNECT: {
            QcState qc = qc_get_state(NULL, 0);

            if (qc == QC_AUTHENTICATED) {
                /* The handshake thread has the token; loading the library is
                 * a few blocking round-trips, so do it here on the main
                 * thread with the "Signed in" frame already on screen. */
                pthread_join(qc_tid, NULL);
                qc_running = 0;
                startup_enter_browse();
                state = STATE_BROWSE;
                start_grid_prefetch(&fb);   /* skipped at launch — see its comment */
                draw_browse(&fb);
                input_drain();
                break;
            }

            /* B retries a dead request rather than making the user relaunch —
             * a code expiring while you walk to another room is the normal
             * way this fails, not an exceptional one. */
            if ((inp & INP_B) && (qc == QC_FAILED || qc == QC_UNAVAILABLE)) {
                if (qc_running) { pthread_join(qc_tid, NULL); qc_running = 0; }
                qc_set_state(QC_STARTING);
                pthread_create(&qc_tid, NULL, quick_connect_thread, NULL);
                qc_running = 1;
                input_drain();
                break;
            }
            if (inp & (INP_A | INP_B)) { g_running = 0; break; }

            draw_quick_connect(&fb);   /* every frame, for the starfield */
            break;
        }

        case STATE_BROWSE: {
            int nav = 0;
            int at_root      = (g_stack[g_stack_depth - 1].kind == FRAME_VIEWS);
            int is_carousel  = at_root && !g_root_list_mode;
            /* Browsing is exactly the case auto-repeat exists for. Safe to
             * fold straight into inp: input_repeat() only ever returns
             * direction bits, so the A/B/SELECT handling further down still
             * sees nothing but real press edges. */
            inp |= nav_repeat;

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
                /* LEFT/RIGHT have nothing else to do in a plain list, so
                 * they're the whole-page jump — the fast way through a long
                 * library. The shoulder buttons do the same thing for pads
                 * where reaching a D-pad direction and a face button at once
                 * is awkward, and because PageUp/PageDown is what a keyboard
                 * user will reach for. */
                int page = visible_rows(&fb);
                if (inp & INP_UP)                   nav |= browse_move_sel(&fb, -1);
                if (inp & INP_DOWN)                 nav |= browse_move_sel(&fb, +1);
                if (inp & (INP_LEFT  | INP_L))      nav |= browse_move_sel(&fb, -page);
                if (inp & (INP_RIGHT | INP_R))      nav |= browse_move_sel(&fb, +page);
            }
            if (at_root) g_root_sel = g_sel;   /* see g_root_sel's own comment */
            if (inp & INP_A && g_item_count > 0) {
                JfItem *it = &g_items[g_sel];
                BrowseFrame *f = &g_stack[g_stack_depth - 1];
                switch (it->type) {
                case JF_TYPE_FOLDER:
                case JF_TYPE_ARTIST:
                    /* The two home rows look like folders but have no parent
                     * to list — they're their own kind of frame. */
                    if (view_is_resume(it))
                        push_frame(FRAME_RESUME, "Continue Watching", NULL, NULL, NULL, NULL);
                    else if (view_is_nextup(it))
                        push_frame(FRAME_NEXTUP, "Next Up", NULL, NULL, NULL, NULL);
                    else
                        push_frame(FRAME_ITEMS, it->name, it->id, NULL, NULL,
                                   it->collection_type[0] ? it->collection_type : f->collection_type);
                    nav = 1;
                    break;
                case JF_TYPE_ALBUM: {
                    /* "Artist / Album" — f->title is still the artist's own
                     * name here (that's the frame we're drilling out of),
                     * so no extra field lookup needed. */
                    char combined[128];
                    snprintf(combined, sizeof(combined), "%s / %s", f->title, it->name);
                    push_frame(FRAME_ITEMS, combined, it->id, NULL, NULL, f->collection_type);
                    nav = 1;
                    break;
                }
                case JF_TYPE_SERIES:
                    push_frame(FRAME_SEASONS, it->name, NULL, it->id, NULL, NULL);
                    nav = 1;
                    break;
                case JF_TYPE_SEASON: {
                    /* "Series / Season", same pattern as Artist / Album. */
                    char combined[128];
                    snprintf(combined, sizeof(combined), "%s / %s", f->title, it->name);
                    push_frame(FRAME_EPISODES, combined, NULL, f->series_id, it->id, NULL);
                    nav = 1;
                    break;
                }
                case JF_TYPE_MOVIE:
                case JF_TYPE_MUSIC_VIDEO:
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

            /* Keeps the top-bar clock live, the over-long-title marquee (see
             * draw_browse) crawling, and the home carousel's grid background
             * (draw_grid_background) scrolling, even with no input at all.
             * The real pacing here is fb_flip()'s own FBIO_WAITFORVSYNC —
             * that's what actually rate-limits this to the display's true
             * refresh (50Hz PAL / 60Hz NTSC) with no tearing, same as every
             * other screen's fb_flip(). The 0.01s check below is just a
             * floor so headless/desktop runs (fb_flip has nothing to wait
             * on there) can't spin this unbounded — safely above any real
             * display's refresh period, so on actual hardware fb_flip's own
             * vsync wait is always what's really pacing it. Was a flat 0.1s
             * (10fps) — fine for the marquee alone, but visibly "janky" for
             * the grid's continuous crawl. g_marquee_px advances by real
             * elapsed time rather than a flat amount per tick, so its speed
             * on screen doesn't change with how often this actually fires. */
            static double last_browse_tick = 0.0;
            if (state == STATE_BROWSE && loop_now - last_browse_tick > 0.01) {
                double dt = last_browse_tick > 0.0 ? loop_now - last_browse_tick : 0.01;
                last_browse_tick = loop_now;
                g_marquee_px += 15.0 * dt;   /* was a flat +1.5 per (assumed 100ms) tick == 15px/sec */
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
                /* Start on whatever the source marks as default, so the track
                 * picker can show a meaningful "current" row before any
                 * choice has been made. */
                g_current_audio_index = default_audio_index();
                play(&fb, g_info_item.id, offset);
                input_drain();
            } else if ((inp & INP_SELECT) && g_info_item.resume_ticks > 0 && !g_info_item.played) {
                info_assets_free();
                playing = 1;
                state = STATE_PLAYING;
                g_current_sub_index = -1;
                g_burned_in_sub_index = -1;
                g_current_audio_index = default_audio_index();
                play(&fb, g_info_item.id, 0.0);
                input_drain();
            }
            break;

        case STATE_PLAYING: {
            int r = player_handle_input(&fb, inp, loop_now);
            if (r == PLAYER_FRAME_HANDLED) continue;
            if (r == PLAYER_FRAME_ENDED) {
                playing = 0;
                state = STATE_BROWSE;
                refetch_frame_keep_selection(&fb);
                draw_browse(&fb);
            }
            break;
        }

        case STATE_PLAYING_AUDIO: {
            JfItem *cur = &g_items[g_audio_queue_pos];
            if (!player_running()) {
                double pos = play_position();
                jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                   (int64_t)(pos * 10000000.0),
                                   playback_watched(cur->runtime_ticks, pos));
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
                double pos = play_position();
                jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                   (int64_t)(pos * 10000000.0),
                                   playback_watched(cur->runtime_ticks, pos));
                player_stop();
                if (g_shuffle_mode) { g_shuffle_mode = 0; fetch_frame(); }
                state = STATE_BROWSE;
                draw_browse(&fb);
                break;
            } else if (inp & INP_A) {
                player_pause_toggle();
            } else if (inp & INP_UP) {
                if (g_audio_queue_pos > 0 && g_items[g_audio_queue_pos - 1].type == JF_TYPE_TRACK) {
                    double pos = play_position();
                    jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                       (int64_t)(pos * 10000000.0),
                                       playback_watched(cur->runtime_ticks, pos));
                    player_stop();
                    play_audio(&fb, g_audio_queue_pos - 1);
                }
            } else if (inp & INP_DOWN) {
                if (g_audio_queue_pos + 1 < g_item_count &&
                    g_items[g_audio_queue_pos + 1].type == JF_TYPE_TRACK) {
                    double pos = play_position();
                    jf_report_stopped(&g_cfg, cur->id, g_play_session_id,
                                       (int64_t)(pos * 10000000.0),
                                       playback_watched(cur->runtime_ticks, pos));
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
                g_now_playing_bg = (g_now_playing_bg + 1) % now_playing_mode_count();
                g_now_playing_bg_shown_until = now_sec() + 1.5;
                if (g_now_playing_bg == NOW_PLAYING_BG_TOASTY && !toasty_is_loaded()) {
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
                report_progress_async(cur->id, g_play_session_id,
                                      (int64_t)(play_position() * 10000000.0), g_paused);
            }
            draw_now_playing(&fb, &g_items[g_audio_queue_pos], play_position());
            break;
        }
        }

        /* One place for the UI sounds rather than a sfx_play() in each of the
         * dozen input branches above: what a click actually means is "the
         * selection moved", and that is a thing to observe once per frame
         * instead of asserting from every path that might have caused it.
         * Keyed on the pid as well as the state because a player that fails
         * to start cleanly drops the app back into the browse list while the
         * process is still alive, and that is precisely the moment not to
         * reach for the sound card — on this device it is not shareable at
         * all (see sfx.c). The in-player track/subtitle menu keeps its
         * selection in a local, so it stays silent for now.
         *
         * `primed` suppresses the very first iteration: was_sel starts at 0
         * and the real selection may not be, which would otherwise click
         * once on its own at startup. */
        sfx_set_enabled(g_player_pid <= 0 &&
                        state != STATE_PLAYING && state != STATE_PLAYING_AUDIO);
        {
            static int primed, was_depth, was_sel;
            static AppState was_state;   /* not int — AppState is unsigned */
            if (primed) {
                if (inp & INP_A)
                    sfx_play(SFX_CONFIRM);
                else if (state == was_state && g_stack_depth == was_depth &&
                         g_sel != was_sel)
                    sfx_play(SFX_NAV);
            }
            primed    = 1;
            was_state = state;
            was_depth = g_stack_depth;
            was_sel   = g_sel;
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
            if (g_fb_flip_count == flips_before) usleep(16000);
        }
    }

    /* The Quick Connect thread can still be polling when the user quits. It
     * checks g_running in 100ms slices, so this waits well under a second —
     * but without it, main() returning would run glibc's exit-time FILE
     * cleanup underneath a thread that is mid-read on a curl pipe. */
    if (qc_running) { pthread_join(qc_tid, NULL); qc_running = 0; }

    bgm_resume();
    /* Before jf_log_close(): the mixer thread logs on its way out. */
    sfx_shutdown();
    jf_log_close();
    player_stop();
    info_assets_free();
    ddr_close();
    cursor_show();
    /* Blanking on the way out leaves the MiSTer showing an empty screen
     * rather than a frozen UI. Headless there's no screen to leave in any
     * state — and doing it anyway would overwrite the final MISTERFIN_FRAME_OUT
     * dump with an all-black frame, i.e. destroy the screenshot the run was
     * for. */
    if (!fb.headless) {
        fb_clear(&fb);
        fb_flip(&fb);
    }
    fb_close(&fb);
    input_close();
    return 0;
}
