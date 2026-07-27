#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
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
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "fb.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#include "font8x8.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ddr.h"
#include "jellyfin.h"
#include "json.h"
#include "subtitles.h"

#define MPLAYER      "/media/fat/misterfin/mplayer-arm"
#define POSTER_TMP   "/tmp/misterfin_poster.img"
/* Separate download scratch path for the background grid-cache prefetch
 * thread (see grid_prefetch_thread()) — it must never share POSTER_TMP with
 * the main thread's own in-flight download (info screen backdrop/logo,
 * browse cover panel, etc.), which would silently clobber whichever one
 * finishes last. */
#define POSTER_TMP_BG "/tmp/misterfin_poster_bg.img"
#define GRID_CACHE_DIR "/media/fat/misterfin/gridcache"
/* Per-item browse-list cover art, cached to the SD card (same idea as the
 * home-screen grid cache above) so scrolling a list reads covers from the
 * card instead of re-downloading each one — no network round-trip on the
 * main thread, so no scroll freeze and no blank-then-appear. A background
 * thread (cover_prefetch_thread) fills the cache for the current list;
 * COVER_TMP_BG is that thread's own download scratch path. */
#define COVER_CACHE_DIR "/media/fat/misterfin/covercache"
#define COVER_TMP_BG   "/tmp/misterfin_cover_bg.img"
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
/* Runs a command with an explicit argv and NO SHELL, optionally capturing
 * stdout. Same reasoning as jf_curl_run in jellyfin.c: the update path
 * interpolates a GitHub-supplied release tag, and with a shell that tag is
 * parsed by /bin/sh. Returns 1 if the command exited 0. */
static int run_no_shell(char *const argv[], char *out, int outlen)
{
    if (out && outlen > 0) out[0] = '\0';

    int pfd[2];
    if (out && pipe(pfd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(pfd[0]); close(pfd[1]); }
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (out) { dup2(pfd[1], STDOUT_FILENO); close(pfd[0]); close(pfd[1]); }
        else if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out) {
        close(pfd[1]);
        int len = 0;
        for (;;) {
            ssize_t got = read(pfd[0], out + len, (size_t)(outlen - 1 - len));
            if (got <= 0) break;
            len += (int)got;
            if (len >= outlen - 1) break;
        }
        out[len] = '\0';
        close(pfd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A release tag is pasted into a download URL and a filename, so it gets a
 * strict allowlist rather than being trusted. Real tags look like "v0.9.3".
 * Returns 1 if the tag is safe to use. */
static int update_tag_is_sane(const char *tag)
{
    if (!tag || !tag[0] || strlen(tag) > 24) return 0;
    for (const char *p = tag; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_';
        if (!ok) return 0;
    }
    return 1;
}

/* Deliberately WITHOUT -k, unlike requests to the user's own Jellyfin server.
 * A self-signed certificate is normal for a home media server, so verification
 * is relaxed there; github.com has a valid certificate and there is no reason
 * to accept anything else. It matters more here than anywhere else in the app:
 * this path decides which binary gets written over misterfin-arm and run at
 * next launch, so accepting a substituted response is accepting a substituted
 * executable. No amount of quoting downstream fixes that. */
/* curl on the stock MiSTer image has no working default CA path — verifying
 * github fails with "unable to get local issuer certificate" even though a
 * perfectly good CA bundle ships in the image, curl just doesn't look there.
 * Point --cacert at the first bundle that exists so TLS verification stays ON
 * (the whole reason the updater dropped -k) instead of silently failing every
 * update check on real hardware. Returns NULL only if none is found. */
static const char *ca_bundle_path(void)
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

static void *update_check_thread(void *arg)
{
    (void)arg;

    const char *ca = ca_bundle_path();
    char *argv[16];
    int n = 0;
    argv[n++] = (char *)"curl";     argv[n++] = (char *)"-fsSL";
    argv[n++] = (char *)"--proto";  argv[n++] = (char *)"=https";
    argv[n++] = (char *)"--tlsv1.2";
    if (ca) { argv[n++] = (char *)"--cacert"; argv[n++] = (char *)ca; }
    argv[n++] = (char *)"--max-time"; argv[n++] = (char *)"8";
    argv[n++] = (char *)"https://api.github.com/repos/puddingstudio/MiSTerFin/releases/latest";
    argv[n] = NULL;

    char buf[8192];
    if (!run_no_shell(argv, buf, sizeof(buf))) goto fail;

    /* Parsed properly rather than scanned for — the same reason every other
     * response in this app is. */
    JsonDoc doc;
    if (!json_parse(&doc, buf)) goto fail;
    char tag[32] = {0};
    json_copy_str(&doc, NULL, "tag_name", tag, sizeof(tag));
    json_free(&doc);

    if (!update_tag_is_sane(tag)) goto fail;

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

typedef enum { INST_IDLE, INST_DOWNLOADING, INST_DONE, INST_FAILED } InstallState;

static InstallState    g_inst_state = INST_IDLE;
static pthread_mutex_t g_inst_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_inst(InstallState s)
{
    pthread_mutex_lock(&g_inst_mutex);
    g_inst_state = s;
    pthread_mutex_unlock(&g_inst_mutex);
}

/* Downloads the tagged release zip, copies assets in place immediately
 * (safe while running), and writes an apply-update script for
 * MiSTerFin.sh to run on the NEXT launch — the running binary/mplayer-arm
 * can't safely overwrite themselves (ETXTBSY) while still executing. */
static void *install_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&g_upd_mutex);
    char tag[32];
    strncpy(tag, g_upd_latest, sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = '\0';
    pthread_mutex_unlock(&g_upd_mutex);

    /* Re-checked here, not just where it was parsed: the tag crosses a mutex
     * and a thread boundary in between, and this is the point where it turns
     * into a URL. */
    if (!update_tag_is_sane(tag)) { set_inst(INST_FAILED); return NULL; }

    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/puddingstudio/MiSTerFin/releases/download/%s/misterfin-%s.zip",
             tag, tag);

    {
        const char *ca = ca_bundle_path();
        char *argv[16];
        int n = 0;
        argv[n++] = (char *)"curl";     argv[n++] = (char *)"-fsSL";
        argv[n++] = (char *)"--proto";  argv[n++] = (char *)"=https";
        argv[n++] = (char *)"--tlsv1.2";
        if (ca) { argv[n++] = (char *)"--cacert"; argv[n++] = (char *)ca; }
        argv[n++] = (char *)"--max-time"; argv[n++] = (char *)"120";
        argv[n++] = url; argv[n++] = (char *)"-o"; argv[n++] = (char *)"/tmp/misterfin-update.zip";
        argv[n] = NULL;
        if (!run_no_shell(argv, NULL, 0)) { set_inst(INST_FAILED); return NULL; }
    }

    {
        char *const argv[] = { (char *)"rm", (char *)"-rf",
                               (char *)"/tmp/misterfin-update/", NULL };
        run_no_shell(argv, NULL, 0);
    }
    {
        /* -qq quiet, -o overwrite. Info-ZIP refuses absolute and ../ paths by
         * default, so a crafted archive can't escape the destination. */
        char *const argv[] = { (char *)"unzip", (char *)"-qq", (char *)"-o",
                               (char *)"/tmp/misterfin-update.zip",
                               (char *)"-d", (char *)"/tmp/misterfin-update/", NULL };
        if (!run_no_shell(argv, NULL, 0)) { set_inst(INST_FAILED); return NULL; }
    }

    static const char *const asset_copies[][3] = {
        { "-r", "/tmp/misterfin-update/misterfin/font/.",    "/media/fat/misterfin/font/"    },
        { "-r", "/tmp/misterfin-update/misterfin/subfont/.", "/media/fat/misterfin/subfont/" },
        { "-r", "/tmp/misterfin-update/misterfin/toasty/.",  "/media/fat/misterfin/toasty/"  },
        { NULL, "/tmp/misterfin-update/misterfin/about.png", "/media/fat/misterfin/about.png" },
    };
    for (size_t i = 0; i < sizeof(asset_copies) / sizeof(asset_copies[0]); i++) {
        char *argv[6];
        int n = 0;
        argv[n++] = (char *)"cp";
        if (asset_copies[i][0]) argv[n++] = (char *)asset_copies[i][0];
        argv[n++] = (char *)asset_copies[i][1];
        argv[n++] = (char *)asset_copies[i][2];
        argv[n]   = NULL;
        run_no_shell(argv, NULL, 0);
    }

    FILE *f = fopen("/tmp/misterfin_apply_update.sh", "w");
    if (f) {
        fprintf(f, "#!/bin/bash\n");
        fprintf(f, "cp /tmp/misterfin-update/misterfin/misterfin-arm /media/fat/misterfin/misterfin-arm\n");
        fprintf(f, "cp /tmp/misterfin-update/misterfin/mplayer-arm   /media/fat/misterfin/mplayer-arm\n");
        fprintf(f, "chmod +x /media/fat/misterfin/misterfin-arm /media/fat/misterfin/mplayer-arm\n");
        fprintf(f, "rm -rf /tmp/misterfin-update/ /tmp/misterfin-update.zip\n");
        fclose(f);
        chmod("/tmp/misterfin_apply_update.sh", 0755);
    }

    set_inst(INST_DONE);
    return NULL;
}

static void about_start_install(void)
{
    pthread_mutex_lock(&g_inst_mutex);
    if (g_inst_state != INST_IDLE) { pthread_mutex_unlock(&g_inst_mutex); return; }
    g_inst_state = INST_DOWNLOADING;
    pthread_mutex_unlock(&g_inst_mutex);

    pthread_t tid;
    pthread_create(&tid, NULL, install_thread, NULL);
    pthread_detach(tid);
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

/* Tried applying the same non-square-pixel correction used for cover art
 * (par_correction()) to font glyph width on NTSC — visually correct per-
 * character, but it doubles every string's on-screen width, which broke
 * layouts never designed for text that wide (confirmed on hardware: home
 * carousel library labels overlapping/running together). Fixing that
 * properly would mean re-deriving card widths/spacing/truncation targets
 * throughout the UI, not just the font — too large a change for what's a
 * cosmetic improvement. Reverted to unconditional passthrough (identical
 * on both PAL and NTSC); the `fb` parameter threaded through text_width()/
 * truncate_to_width()/draw_wrapped() is otherwise unused now but left in
 * place rather than unwinding ~40 call sites for no behavioral gain. */
static int font_scale_x(FBDev *fb, int scale)
{
    (void)fb;
    return scale;
}

/* Text is UTF-8. One glyph cell is drawn per code point (not per byte), so
 * accented Latin (é, ã, ç, ñ, ü, ...) renders properly instead of as one
 * cell per raw byte. ASCII comes from font8x8_basic, U+00A0-U+00FF from
 * font8x8_ext_latin; anything else (CJK/Cyrillic/Greek) shows '?'. Titles/
 * overviews are pre-normalised by jf_text_to_display so only code points we
 * can actually draw ever reach here. */
static unsigned utf8_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p, cp; int extra;
    if      (c < 0x80)           { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { *s += 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

static const uint8_t *glyph_for_cp(unsigned cp)
{
    if (cp < 0x80)                return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF) return font8x8_ext_latin[cp - 0xA0];
    return font8x8_basic['?'];
}

/* Number of code points in the first nbytes of s (stops at NUL). */
static int cp_count(const char *s, int nbytes)
{
    int n = 0; const char *end = s + nbytes;
    while (s < end && *s) { utf8_cp(&s); n++; }
    return n;
}

/* Cut s in place to at most keep_cols code points (at a code-point boundary,
 * no ellipsis added). */
static void truncate_cp(char *s, int keep_cols)
{
    const char *p = s; int i = 0;
    while (*p && i < keep_cols) { utf8_cp(&p); i++; }
    s[(int)(p - s)] = '\0';
}

static void draw_char(FBDev *fb, int x, int y, unsigned cp, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int sx = font_scale_x(fb, scale);
    const uint8_t *glyph = glyph_for_cp(cp);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((bits >> col) & 1)
                fb_fill_rect_alpha(fb, x + col*sx, y + row*scale,
                                   sx, scale, r, g, b, 255);
        }
    }
}

static void draw_text(FBDev *fb, int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int sx = font_scale_x(fb, scale);
    while (*s) { unsigned cp = utf8_cp(&s); draw_char(fb, x, y, cp, scale, r, g, b); x += 8*sx; }
}

static int text_width(FBDev *fb, const char *s, int scale)
{
    int n = 0;
    while (*s) { utf8_cp(&s); n++; }
    return n * 8 * font_scale_x(fb, scale);
}

/* Like draw_text, but skips any character whose whole glyph cell doesn't
 * fit within [clip_x0, clip_x1) — used for the scrolling browse title
 * marquee so it doesn't run into the clock/spinner area to its right (or
 * off the left edge while scrolling). Character-granularity, not
 * pixel-perfect, but fine for a continuously-moving marquee. */
static void draw_text_clipped(FBDev *fb, int x, int y, const char *s, int scale,
                               uint8_t r, uint8_t g, uint8_t b, int clip_x0, int clip_x1)
{
    while (*s) {
        unsigned cp = utf8_cp(&s);
        if (x >= clip_x0 && x + 8 * scale <= clip_x1)
            draw_char(fb, x, y, cp, scale, r, g, b);
        x += 8 * scale;
    }
}

/* Truncates s in-place (with a trailing "...") so it fits within max_w
 * pixels at the given scale — draw_text itself doesn't clip, so a long
 * title would otherwise run into whatever's to its right. */
static void truncate_to_width(FBDev *fb, char *s, int scale, int max_w)
{
    int max_chars = max_w / (8 * font_scale_x(fb, scale));
    if (max_chars < 1) { s[0] = '\0'; return; }
    int n = 0; { const char *p = s; while (*p) { utf8_cp(&p); n++; } }
    if (n <= max_chars) return;
    if (max_chars > 3) { truncate_cp(s, max_chars - 3); strcat(s, "..."); }
    else               { truncate_cp(s, max_chars); }
}

static int draw_wrapped(FBDev *fb, int x, int y, const char *text,
                         int scale, int max_w, int max_lines,
                         uint8_t r, uint8_t g, uint8_t b)
{
    int cols    = max_w / (8 * font_scale_x(fb, scale));
    int line_h  = 8 * scale + 2;
    char line[256] = {0};
    int  li         = 0;   /* bytes in `line` */
    int  lcols      = 0;   /* display columns in `line` */
    int  drawn      = 0;
    const char *p   = text;

    while (*p && drawn < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *we = p;
        while (*we && *we != ' ') we++;
        int wlen  = (int)(we - p);
        int wcols = cp_count(p, wlen);
        if (lcols > 0 && lcols + 1 + wcols > cols) {
            int is_last = (drawn == max_lines - 1);
            if (is_last && *we) {
                if (lcols > cols - 3) truncate_cp(line, cols - 3 > 0 ? cols - 3 : 0);
                strcat(line, "...");
            }
            draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
            drawn++; li = 0; lcols = 0; memset(line, 0, sizeof(line));
        }
        if (lcols > 0 && li < (int)sizeof(line) - 2) { line[li++] = ' '; lcols++; }
        if (li + wlen < (int)sizeof(line) - 1) { memcpy(line+li, p, wlen); li += wlen; lcols += wcols; }
        p = we;
    }
    if (li > 0 && drawn < max_lines) {
        draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
        drawn++;
    }
    return drawn * line_h;
}

/* ── input (evdev gamepad, same model as MiSTerDVD) ─────────────────────── */

/* Bit-array helpers for the evdev state ioctls below (linux/input.h returns
 * these as an array of unsigned long). */
#define INPUT_BITS_PER_LONG  (8 * (int)sizeof(unsigned long))
#define INPUT_NLONGS(n)      (((n) + INPUT_BITS_PER_LONG - 1) / INPUT_BITS_PER_LONG)
#define INPUT_TEST_BIT(arr, bit) \
    ((arr)[(bit) / INPUT_BITS_PER_LONG] & (1UL << ((bit) % INPUT_BITS_PER_LONG)))

/* Was 8. A MiSTer has well over that many /dev/input/event* nodes once
 * keyboards, mice, the MiSTer virtual input device and a pad or two are
 * present — and a single pad often exposes several. With a hard cap and no
 * eviction, which devices got opened came down to readdir order, which is
 * filesystem order rather than anything meaningful. */
#define MAX_INPUT_FDS 32
static int  input_fds[MAX_INPUT_FDS];
static int  input_swap_ab[MAX_INPUT_FDS];
static int  input_is_virtual[MAX_INPUT_FDS];
static char input_names[MAX_INPUT_FDS][32];   /* e.g. "event0" — see input_open() */
static int  input_count = 0;
/* MISTERFIN_INPUT_DEBUG=1 — prints every incoming event with its device, raw
 * code, and what it mapped to (or why it was dropped). MiSTer's input routing
 * is genuinely unusual: it grabs directly-wired USB pads exclusively and
 * re-emits them on a synthetic "MiSTer virtual input" device, so which button
 * arrives on which node, under which code, varies by pad and by connection
 * type. Guessing at that from a bug report is hopeless; this makes it a
 * two-minute answer. Output goes to stderr, so run over SSH — it doesn't
 * touch the framebuffer UI. */
static int  input_debug = 0;


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

static const char *inp_bit_name(int bit)
{
    switch (bit) {
    case INP_UP: return "UP";       case INP_DOWN:  return "DOWN";
    case INP_LEFT: return "LEFT";   case INP_RIGHT: return "RIGHT";
    case INP_A: return "A";         case INP_B:     return "B";
    case INP_START: return "START"; case INP_SELECT: return "SELECT";
    case INP_L: return "L";         case INP_R:      return "R";
    default: return "?";
    }
}

/* Safe to call repeatedly (see the periodic re-scan in the main loop) —
 * skips any /dev/input/eventN already tracked, only opening ones that are
 * new since the last call (a reconnected wireless pad, for example, can
 * come back as a fresh node with a different number). */
static void stdin_input_open(void);   /* forward decls — desktop backends, defined below */
static void script_open(void);
static void stdin_input_restore(void);
static void stdin_input_drain(void);

static void input_open(void)
{
    /* Desktop backends come up alongside (not instead of) evdev — both are
     * no-ops unless their own env var asked for them, so this stays exactly
     * the old behavior on real hardware. Safe to call repeatedly, same as
     * the evdev scan below (see this function's own header comment). */
    if (getenv("MISTERFIN_STDIN")) stdin_input_open();
    if (getenv("MISTERFIN_INPUT_DEBUG")) input_debug = 1;
    script_open();

    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && input_count < MAX_INPUT_FDS) {
        if (strncmp(e->d_name, "event", 5)) continue;

        int already = 0;
        for (int i = 0; i < input_count; i++)
            if (!strcmp(input_names[i], e->d_name)) { already = 1; break; }
        if (already) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* Skip anything with neither buttons nor axes. A system has plenty of
         * such nodes (power buttons, lid switches, accelerometers, the
         * console's own pseudo-devices) and every one of them used to occupy
         * a slot that a real controller then couldn't have. */
        unsigned long evbits[INPUT_NLONGS(EV_MAX + 1)];
        memset(evbits, 0, sizeof(evbits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
            (!INPUT_TEST_BIT(evbits, EV_KEY) && !INPUT_TEST_BIT(evbits, EV_ABS))) {
            close(fd);
            continue;
        }

        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        input_swap_ab[input_count]    = device_needs_ab_swap(name);
        input_is_virtual[input_count] = device_is_mister_virtual(name);
        strncpy(input_names[input_count], e->d_name, sizeof(input_names[0]) - 1);
        input_fds[input_count++] = fd;
        if (input_debug)
            fprintf(stderr, "[input] opened %s \"%s\"%s (%d tracked)\n",
                    e->d_name, name,
                    device_is_mister_virtual(name) ? " [MiSTer virtual]" : "",
                    input_count);
    }
    closedir(d);
}

static void input_repeat_reset(void);   /* forward decl — defined with the repeat logic below */

/* Drops a device that has disappeared, compacting the parallel arrays.
 *
 * Without this, a controller that disconnects and comes back — which is
 * routine for anything wireless, and is exactly what happens after a pad
 * freezes and gets reconnected — leaves its dead entry holding a slot
 * forever while the live node needs a new one. Slots leak on every reconnect
 * until nothing new can be opened at all, and buttons simply stop arriving
 * with no visible cause. */
static void input_drop_slot(int i)
{
    if (input_debug)
        fprintf(stderr, "[input] %s went away, releasing slot\n", input_names[i]);
    close(input_fds[i]);
    for (int j = i; j < input_count - 1; j++) {
        input_fds[j]        = input_fds[j + 1];
        input_swap_ab[j]    = input_swap_ab[j + 1];
        input_is_virtual[j] = input_is_virtual[j + 1];
        memcpy(input_names[j], input_names[j + 1], sizeof(input_names[0]));
    }
    input_count--;
}

static void input_drain(void)
{
    struct input_event ev;
    for (int i = 0; i < input_count; i++)
        while (read(input_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {}
    input_repeat_reset();
    stdin_input_drain();
}

static void input_close(void)
{
    for (int i = 0; i < input_count; i++) close(input_fds[i]);
    input_count = 0;
    stdin_input_restore();
}

/* ── desktop keyboard backend (stdin, raw mode) ──────────────────────────
 * Off-hardware there are no /dev/input/eventN gamepads to read, so the same
 * INP_* masks are sourced from the controlling terminal instead. Enabled by
 * MISTERFIN_STDIN=1 (interactive) or implicitly by MISTERFIN_KEYS (scripted,
 * below) — never on the MiSTer, where the evdev path above is the real one.
 *
 * Terminals do their own key repeat when a key is held, so this backend gets
 * auto-repeat for free and doesn't participate in the evdev held-state
 * repeat logic. */
static int  stdin_enabled = 0;
static struct termios stdin_saved_termios;
static int  stdin_termios_saved = 0;

static void stdin_input_restore(void)
{
    if (!stdin_termios_saved) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_saved_termios);
    stdin_termios_saved = 0;
}

static void stdin_input_open(void)
{
    if (stdin_enabled || !isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &stdin_saved_termios) != 0) return;

    struct termios raw = stdin_saved_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);   /* byte-at-a-time, no local echo */
    raw.c_cc[VMIN]  = 0;                          /* fully non-blocking reads */
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;

    stdin_termios_saved = 1;
    stdin_enabled       = 1;
    atexit(stdin_input_restore);   /* also covers the on_fatal/_exit paths' terminal state */
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
}

/* Discards buffered keystrokes typed while a blocking operation was running,
 * matching what input_drain() does for the evdev queues. Scripted playback
 * is deliberately unaffected — it's paced off the wall clock rather than
 * buffered, so there's nothing to drop. */
static void stdin_input_drain(void)
{
    if (!stdin_enabled) return;
    unsigned char buf[64];
    while (read(STDIN_FILENO, buf, sizeof(buf)) > 0) {}
}

/* Maps one already-decoded terminal key to an INP_* bit. Escape sequences
 * (arrows, PageUp/Down, Home) are decoded by the caller and passed in as the
 * synthetic codes below, which are deliberately outside the ASCII range so
 * they can't collide with a real typed character. */
#define TK_UP     0x100
#define TK_DOWN   0x101
#define TK_LEFT   0x102
#define TK_RIGHT  0x103
#define TK_PGUP   0x104
#define TK_PGDN   0x105
#define TK_HOME   0x106
#define TK_ESC    0x107

static int stdin_key_to_mask(int key)
{
    switch (key) {
    case TK_UP:                 return INP_UP;
    case TK_DOWN:               return INP_DOWN;
    case TK_LEFT:               return INP_LEFT;
    case TK_RIGHT:              return INP_RIGHT;
    /* Same pairing the evdev path uses: Enter/X confirm, Esc/Z back. */
    case '\r': case '\n':
    case 'x': case 'X':         return INP_A;
    case TK_ESC:
    case 0x7f: case '\b':
    case 'z': case 'Z':         return INP_B;
    case '\t':                  return INP_SELECT;
    case TK_HOME: case 'p':     return INP_START;
    case TK_PGUP: case '[':     return INP_L;
    case TK_PGDN: case ']':     return INP_R;
    default:                    return 0;
    }
}

/* Decodes whatever bytes are pending on stdin into an INP_* mask. 'q' quits
 * outright — there's no window manager to close off-hardware, and Esc is
 * already spoken for as "back". */
static int stdin_poll(void)
{
    if (!stdin_enabled) return 0;

    unsigned char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return 0;

    int mask = 0;
    for (ssize_t i = 0; i < n; i++) {
        int key = buf[i];
        if (key == 'q') { g_running = 0; continue; }

        /* CSI sequences arrive as one contiguous burst in the same read, so
         * looking ahead within this buffer is enough — a bare ESC (nothing
         * following it) is a real Esc keypress. */
        if (key == 0x1b) {
            if (i + 2 < n && buf[i + 1] == '[') {
                unsigned char c = buf[i + 2];
                i += 2;
                switch (c) {
                case 'A': key = TK_UP;    break;
                case 'B': key = TK_DOWN;  break;
                case 'C': key = TK_RIGHT; break;
                case 'D': key = TK_LEFT;  break;
                case 'H': key = TK_HOME;  break;
                /* "5~"/"6~"/"1~" — swallow the trailing '~' too */
                case '5': key = TK_PGUP; if (i + 1 < n && buf[i+1] == '~') i++; break;
                case '6': key = TK_PGDN; if (i + 1 < n && buf[i+1] == '~') i++; break;
                case '1': key = TK_HOME; if (i + 1 < n && buf[i+1] == '~') i++; break;
                default:  continue;   /* some other CSI sequence — ignore it */
                }
            } else {
                key = TK_ESC;
            }
        }
        mask |= stdin_key_to_mask(key);
    }
    return mask;
}

/* ── scripted key playback (MISTERFIN_KEYS) ──────────────────────────────
 * A comma-separated list of key names, each optionally followed by ":<ms>"
 * for how long to wait after it before the next one (default SCRIPT_STEP_MS)
 * — e.g. MISTERFIN_KEYS="right,right,a:800,down,down".
 *
 * Reproducible screenshots without a human at the keyboard: pair it with
 * MISTERFIN_FRAME_OUT and the raw file holds whatever was on screen when the
 * script finished. The app quits once the queue drains (that's the point —
 * it's for automation), unless MISTERFIN_KEYS_HOLD=1 asks it to stay up. */
#define SCRIPT_MAX      256
#define SCRIPT_STEP_MS  400

typedef struct { int mask; int delay_ms; } ScriptKey;
static ScriptKey script_keys[SCRIPT_MAX];
static int    script_count = 0, script_pos = 0;
static double script_next_at = 0.0;
static int    script_hold = 0;

static int script_name_to_mask(const char *name)
{
    if (!strcmp(name, "up"))     return INP_UP;
    if (!strcmp(name, "down"))   return INP_DOWN;
    if (!strcmp(name, "left"))   return INP_LEFT;
    if (!strcmp(name, "right"))  return INP_RIGHT;
    if (!strcmp(name, "a"))      return INP_A;
    if (!strcmp(name, "b"))      return INP_B;
    if (!strcmp(name, "select")) return INP_SELECT;
    if (!strcmp(name, "start"))  return INP_START;
    if (!strcmp(name, "l"))      return INP_L;
    if (!strcmp(name, "r"))      return INP_R;
    if (!strcmp(name, "wait"))   return 0;   /* pure delay, no button */
    fprintf(stderr, "MISTERFIN_KEYS: unknown key \"%s\"\n", name);
    return 0;
}

/* input_open() re-runs every few seconds to pick up hotplugged pads, so this
 * has to be idempotent — without the latch, each rescan would re-parse the
 * script and rewind it to the start, looping forever. */
static void script_open(void)
{
    static int script_loaded = 0;
    if (script_loaded) return;
    script_loaded = 1;

    const char *spec = getenv("MISTERFIN_KEYS");
    if (!spec || !*spec) return;
    const char *hold = getenv("MISTERFIN_KEYS_HOLD");
    script_hold = (hold && *hold && strcmp(hold, "0") != 0);

    char list[1024];
    strncpy(list, spec, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    for (char *tok = strtok(list, ","); tok && script_count < SCRIPT_MAX;
         tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        int delay = SCRIPT_STEP_MS;
        char *colon = strchr(tok, ':');
        if (colon) { *colon = '\0'; delay = atoi(colon + 1); }
        script_keys[script_count].mask     = script_name_to_mask(tok);
        script_keys[script_count].delay_ms = delay > 0 ? delay : SCRIPT_STEP_MS;
        script_count++;
    }
    /* First key fires after one step, so the startup fetch has a moment to
     * land before the script starts pressing things. */
    if (script_count > 0) script_next_at = now_sec() + SCRIPT_STEP_MS / 1000.0;
}

static int script_poll(void)
{
    if (script_count == 0) return 0;
    double t = now_sec();
    if (t < script_next_at) return 0;

    if (script_pos >= script_count) {
        if (!script_hold) g_running = 0;
        return 0;
    }
    ScriptKey *k = &script_keys[script_pos++];
    script_next_at = t + k->delay_ms / 1000.0;
    return k->mask;
}

static int input_poll(void)
{
    struct input_event ev;
    int mask = stdin_poll() | script_poll();
    for (int i = 0; i < input_count; i++) {
        /* Reading a removed device fails with ENODEV; that's how a
         * disconnect is noticed, since evdev has no other notification.
         * Dropping the slot here is what lets the same pad be picked up
         * again by the next rescan. */
        ssize_t got;
        while ((got = read(input_fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            /* Press edges only. Releases don't need tracking here: auto-repeat
             * reads the device's real current state instead of reconstructing
             * it from edges (see input_repeat). The kernel's own key repeat
             * (value 2) is ignored too — it only fires for real keyboards,
             * never for a gamepad button or D-pad hat, and runs at whatever
             * rate the console is configured for. */
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
                    if (input_debug)
                        fprintf(stderr, "[input] %s EV_KEY code=%d val=%d -> DROPPED "
                                        "(not trusted from MiSTer virtual input)\n",
                                input_names[i], code, ev.value);
                    continue;
                }
                int bit = 0;
                switch (code) {
                case BTN_EAST:               bit = INP_A;      break;
                case BTN_SOUTH:              bit = INP_B;      break;
                /* Enter/Esc are the intuitive confirm/cancel pair; X/Z are
                 * the de facto SNES-emulator standard (RetroArch/SNES9x
                 * default keyboard mapping) matching the SNES pad's A
                 * (right) / B (bottom) positions — both work. */
                case KEY_ENTER:
                case KEY_X:                  bit = INP_A;      break;
                case KEY_ESC:
                case KEY_BACK:
                case KEY_BACKSPACE:
                case KEY_Z:                  bit = INP_B;      break;
                case BTN_START: case KEY_PAUSE: case KEY_HOME: bit = INP_START;  break;
                case BTN_SELECT: case KEY_TAB:  bit = INP_SELECT; break;
                case KEY_UP:                     bit = INP_UP;    break;
                case KEY_DOWN:                   bit = INP_DOWN;  break;
                case KEY_LEFT:                   bit = INP_LEFT;  break;
                case KEY_RIGHT:                  bit = INP_RIGHT; break;
                case BTN_TL: case KEY_PAGEUP:    bit = INP_L;     break;
                case BTN_TR: case KEY_PAGEDOWN:  bit = INP_R;     break;
                }
                if (input_debug)
                    fprintf(stderr, "[input] %s EV_KEY code=%d val=%d -> %s\n",
                            input_names[i], code, ev.value,
                            bit ? inp_bit_name(bit) : "unmapped");
                mask |= bit;
            } else if (ev.type == EV_ABS) {
                if (input_debug && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y))
                    fprintf(stderr, "[input] %s EV_ABS %s value=%d\n",
                            input_names[i],
                            ev.code == ABS_HAT0X ? "HAT0X" : "HAT0Y", ev.value);
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
        if (got < 0 && (errno == ENODEV || errno == EBADF)) {
            input_drop_slot(i);
            i--;            /* the slot now holds the next device */
        }
    }
    return mask;
}

/* ── navigation auto-repeat ──────────────────────────────────────────────────
 * Holding a direction should keep scrolling instead of demanding one press
 * per row — a library of any size was otherwise a genuine repetitive-strain
 * hazard to get through.
 *
 * Deliberately NOT folded into input_poll()'s return value: repeats are only
 * wanted where the action is "move a cursor". Applying them everywhere would
 * make a held UP skip through music tracks at ten a second on the now-playing
 * screen, and a held LEFT pile up an enormous accumulated video seek. Callers
 * that want repeat OR this in explicitly; everything else keeps seeing clean
 * press edges only.
 *
 * Two rates: a slower one to start with (so a deliberate single-row nudge
 * doesn't overshoot), then faster once it's clear the direction is being held
 * on purpose, which is what makes crossing a few hundred rows bearable. */
#define REPEAT_DELAY_SEC  0.35   /* hold this long before repeating at all */
#define REPEAT_SLOW_SEC   0.11
#define REPEAT_FAST_SEC   0.045
#define REPEAT_RAMP_AFTER 6      /* repeats at the slow rate before speeding up */

static int    repeat_mask  = 0;      /* direction currently repeating, 0 = none */
static double repeat_next  = 0.0;
static int    repeat_count = 0;
static int    repeat_suppressed = 0; /* see input_repeat_reset */

static void input_repeat_reset(void)
{
    repeat_mask  = 0;
    repeat_next  = 0.0;
    repeat_count = 0;
    /* Called from input_drain, i.e. right after a screen change. Whatever is
     * physically held at that moment shouldn't immediately start scrolling
     * the screen you just arrived at, so repeat stays parked until the user
     * lets go of everything. */
    repeat_suppressed = 1;
}

/* Asks each device what it is ACTUALLY holding right now, via EVIOCGKEY /
 * EVIOCGABS, rather than reconstructing it from the press/release stream.
 *
 * This is a correctness fix, not an optimisation. Reconstructing held state
 * from edges means a single missed release leaves a direction stuck "down"
 * forever — which showed up on hardware as auto-repeat that wouldn't stop
 * until the button was pressed again. There are several ways to miss one
 * here: input_drain() deliberately discards pending events at every screen
 * change, a hotplugged pad can be reopened under a new event node mid-press
 * leaving a stale entry nothing ever clears, and MiSTer's OSD echoes pad
 * input onto a second virtual device whose event pairing this code does not
 * control. Reading the state directly makes all of those unrepresentable:
 * there is no accumulated state to go wrong, and a device that has gone away
 * simply fails the ioctl and contributes nothing. */
static int input_nav_held(void)
{
    int mask = 0;
    for (int i = 0; i < input_count; i++) {
        unsigned long keys[INPUT_NLONGS(KEY_MAX + 1)];
        memset(keys, 0, sizeof(keys));
        if (ioctl(input_fds[i], EVIOCGKEY(sizeof(keys)), keys) >= 0) {
            if (INPUT_TEST_BIT(keys, KEY_UP))    mask |= INP_UP;
            if (INPUT_TEST_BIT(keys, KEY_DOWN))  mask |= INP_DOWN;
            if (INPUT_TEST_BIT(keys, KEY_LEFT))  mask |= INP_LEFT;
            if (INPUT_TEST_BIT(keys, KEY_RIGHT)) mask |= INP_RIGHT;
        }
        /* D-pads arrive as a hat axis rather than as keys. A device without
         * these axes just fails the ioctl or reports 0, both of which mean
         * "nothing held" — no need to probe capabilities first. */
        struct input_absinfo abs;
        if (ioctl(input_fds[i], EVIOCGABS(ABS_HAT0Y), &abs) >= 0) {
            if (abs.value < 0) mask |= INP_UP;
            if (abs.value > 0) mask |= INP_DOWN;
        }
        if (ioctl(input_fds[i], EVIOCGABS(ABS_HAT0X), &abs) >= 0) {
            if (abs.value < 0) mask |= INP_LEFT;
            if (abs.value > 0) mask |= INP_RIGHT;
        }
    }
    return mask;
}

/* Returns the nav bits that should act as though freshly pressed this tick
 * because they're being held down. Call once per main-loop iteration. */
static int input_repeat(void)
{
    int held = input_nav_held();

    if (repeat_suppressed) {
        if (held) return 0;      /* still held over from before the screen change */
        repeat_suppressed = 0;   /* released — normal service resumes */
    }

    double t = now_sec();

    /* Any change in which direction is held restarts the delay — including
     * releasing one of two simultaneously held directions, which is the
     * right call: the surviving direction is then effectively a new press. */
    if (held != repeat_mask) {
        repeat_mask  = held;
        repeat_count = 0;
        repeat_next  = held ? t + REPEAT_DELAY_SEC : 0.0;
        return 0;
    }
    if (!held || t < repeat_next) return 0;

    repeat_next = t + (repeat_count >= REPEAT_RAMP_AFTER ? REPEAT_FAST_SEC : REPEAT_SLOW_SEC);
    repeat_count++;
    return held;
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
    char      series_id[JF_ID_LEN];  /* FRAME_SEASONS / FRAME_EPISODES */
    char      season_id[JF_ID_LEN];  /* FRAME_EPISODES */
} BrowseFrame;

static JfConfig   g_cfg;
static BrowseFrame g_stack[MAX_STACK];
static int         g_stack_depth = 0;

static JfItem g_items[JF_MAX_ITEMS];
static int    g_item_count = 0;
static int    g_sel = 0, g_scroll = 0;
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
 * Toasty Squadron sprites (see draw_toasty). Persists for the whole app
 * session, same as g_root_list_mode. Index 2 (Nebula) is an immersive mode: it hides
 * the clock/title/timeline/VU and just shows an enlarged centered cover over
 * the effect (see draw_now_playing). */
#define NOW_PLAYING_BG_COUNT 4
#define NOW_PLAYING_BG_NEBULA  2
static int    g_now_playing_bg = 0;
static const char *NOW_PLAYING_BG_NAMES[] = { "Starfield", "Rain", "Nebula", "Toasty Squadron" };
/* Label shows briefly on change then disappears, rather than sitting on
 * screen permanently — set to now_sec()+1.5 wherever g_now_playing_bg
 * changes (see the STATE_PLAYING_AUDIO SELECT handling), 0 = not shown. */
static double g_now_playing_bg_shown_until = 0.0;
/* In the immersive backgrounds (Nebula/Toasty) the track title is otherwise
 * hidden, so on each track change it's flashed at the top for a few seconds
 * then disappears — set to now_sec()+N wherever a new track starts playing
 * (see play_audio), 0 = not shown. */
static double g_np_title_shown_until = 0.0;

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

/* Like load_image_tmp but KEEPS the file — for reading a persistent cache
 * file off the SD card (the cover cache), where deleting it would defeat the
 * whole point. */
static uint8_t *load_image_keep(const char *path, int *w, int *h)
{
    int channels = 0;
    return stbi_load(path, w, h, &channels, 4);
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

/* The two home rows are presented as extra cards on the library carousel, but
 * they aren't libraries — no ParentId to list, no collection type, and their
 * contents change every time something is watched. Everything that would
 * normally reach for the server via a view id has to route around them. */
static int view_is_resume(const JfItem *v) { return v->synthetic == JF_SYNTH_RESUME; }
static int view_is_nextup(const JfItem *v) { return v->synthetic == JF_SYNTH_NEXTUP; }
static int view_is_synthetic(const JfItem *v) { return v->synthetic != 0; }

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
        g_item_count = jf_list_items(&g_cfg, f->parent_id, start_index,
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
     * reads covers off the card, never off the network. */
    start_cover_prefetch();
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

/* Pixel-aspect-ratio correction: our framebuffer's pixels aren't square on
 * a 4:3 CRT/component display, so a source image's width needs stretching
 * relative to naive (sw/sh)*dh math to look right. Derived from the live
 * framebuffer size rather than hardcoded, so it's automatically correct at
 * any active-line count (PAL 288, NTSC 240, ...) instead of only the one
 * resolution it happened to be tuned against. At fb->height=288 this comes
 * out to exactly 5/3 — MiSTerDVD's original proven PAL correction factor
 * (confirmed unchanged: was hardcoded 5.0/3.0 before this generalization). */
static double par_correction(FBDev *fb)
{
    return (3.0 * fb->width) / (4.0 * fb->height);
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

/* Now-playing background effect #2 (index NOW_PLAYING_BG_NEBULA) — "Nebula",
 * our own audio-reactive plasma visualizer. It's an ORIGINAL effect written
 * from scratch, inspired by Ryan Geiss's classic feedback visualizer but not
 * a port of any of his code — so it carries no third-party licensing.
 *
 * How it works, and why it's cheap enough for a Cortex-A9 at ~60fps:
 *  - A small NEBULA_W x NEBULA_H intensity field is kept between frames. Each
 *    frame it's resampled through a gentle zoom + slow rotation and decayed
 *    a little — that feedback is what turns injected shapes into flowing
 *    plasma trails (the Geiss signature).
 *  - The live PCM (samples: n int16s, left half then right half — see
 *    read_af_samples) is injected as a bright centered scope line, and its
 *    energy speeds up the swirl and palette so the whole thing reacts to
 *    the music.
 *  - The field maps through a time-cycling palette and is nearest-neighbour
 *    upscaled to the framebuffer via precomputed x/y maps (no per-pixel
 *    divide in the hot loop). All the per-pixel cost is one field this small
 *    plus one full-screen palette lookup. */
#define NEBULA_W     160
#define NEBULA_H      90
#define NEBULA_MAXW  720
#define NEBULA_MAXH  576
#define NEBULA_NATTR   1   /* one gentle "circular attractor" — see the warp loop */

static uint8_t  nebula_buf[2][NEBULA_W * NEBULA_H];
static int      nebula_cur = 0;
static double   nebula_pal_phase = 0.0;
static double   nebula_hue = 0.0;   /* eased toward the current 15s scheme */
static double   nebula_swirl = 0.0;
static double   nebula_t = 0.0;   /* attractor-motion clock */
static int      nebula_xmap[NEBULA_MAXW], nebula_ymap[NEBULA_MAXH];
static int      nebula_map_w = 0, nebula_map_h = 0;

static void draw_nebula(FBDev *fb, const int16_t *samples, int n)
{
    if (fb->width > NEBULA_MAXW || fb->height > NEBULA_MAXH) return;   /* OOB guard */

    if (nebula_map_w != fb->width) {
        for (int x = 0; x < fb->width; x++) nebula_xmap[x] = x * NEBULA_W / fb->width;
        nebula_map_w = fb->width;
    }
    if (nebula_map_h != fb->height) {
        for (int y = 0; y < fb->height; y++) nebula_ymap[y] = (y * NEBULA_H / fb->height) * NEBULA_W;
        nebula_map_h = fb->height;
    }

    uint8_t *prev = nebula_buf[nebula_cur];
    uint8_t *next = nebula_buf[nebula_cur ^ 1];
    nebula_cur ^= 1;

    /* Mean-abs of this frame's left channel, normalized to 0..1. */
    int half = n / 2;
    double energy = 0.0;
    for (int i = 0; i < half; i++) { int s = samples[i]; energy += s < 0 ? -s : s; }
    if (half > 0) energy /= (double)half * 32768.0;
    if (energy > 1.0) energy = 1.0;

    /* Feedback resample: zoom + slow rotation, both breathing with the audio,
     * then decay. */
    /* Global motion: gentle zoom + slow rotation, both breathing with the
     * audio. */
    nebula_swirl += 0.010 + energy * 0.05;
    double theta = 0.022 * sin(nebula_swirl);
    double zoom  = 1.0 - (0.010 + energy * 0.018);   /* <1 => content flows */
    double ct = cos(theta) / zoom, st = sin(theta) / zoom;
    double cx = NEBULA_W / 2.0, cy = NEBULA_H / 2.0;
    unsigned decay = 243;   /* /256 per frame */

    /* On top of the global spin, one gentle "circular attractor" drifting
     * behind the cover adds a small bounded radial pull + tangential swirl to
     * where the feedback is sampled — just enough to curl the waveform,
     * deliberately weak so it doesn't suck the whole image into a whirlpool.
     * Louder audio strengthens it a little. */
    nebula_t += 0.006 + energy * 0.02;
    static const double asw[NEBULA_NATTR] = { 0.26 };   /* swirl sign/strength */
    double ax[NEBULA_NATTR], ay[NEBULA_NATTR], asr[NEBULA_NATTR];
    for (int i = 0; i < NEBULA_NATTR; i++) {
        /* Small drift around the centre so the single vortex sits behind the
         * cover and stays gentle — a weak pull that curls the waveform a
         * little rather than sucking it into a whirlpool. */
        ax[i] = cx + (NEBULA_W * 0.10) * sin(nebula_t * 0.43);
        ay[i] = cy + (NEBULA_H * 0.10) * cos(nebula_t * 0.37);
        asr[i] = 12.0 + energy * 16.0;
    }

    for (int y = 0; y < NEBULA_H; y++) {
        double dy = y - cy;
        double bx = cx - dy * st;
        double by = cy + dy * ct;
        uint8_t *nrow = next + y * NEBULA_W;
        for (int x = 0; x < NEBULA_W; x++) {
            double dx = x - cx;
            double fx = bx + dx * ct;
            double fy = by + dx * st;
            for (int i = 0; i < NEBULA_NATTR; i++) {
                double rx = ax[i] - x, ry = ay[i] - y;
                double fo = asr[i] / (rx * rx + ry * ry + 24.0);
                fx += rx * fo + (-ry) * fo * asw[i];
                fy += ry * fo + ( rx) * fo * asw[i];
            }
            int sx = (int)fx, sy = (int)fy;
            uint8_t v = 0;
            if ((unsigned)sx < (unsigned)NEBULA_W && (unsigned)sy < (unsigned)NEBULA_H)
                v = prev[sy * NEBULA_W + sx];
            nrow[x] = (uint8_t)((v * decay) >> 8);
        }
    }

    /* Waveform injected additively so overlapping passes build up brightness
     * and the feedback smears it into plasma over the following frames. */
    if (half > 0) {
        double amp = NEBULA_H * 0.40;
        for (int x = 0; x < NEBULA_W; x++) {
            int s  = samples[x * half / NEBULA_W];
            int yy = (int)(cy + (double)s / 32768.0 * amp);
            if ((unsigned)yy < (unsigned)NEBULA_H) {
                uint8_t *c = &next[yy * NEBULA_W + x];
                int t = *c + 170; *c = t > 255 ? 255 : (uint8_t)t;
                if (yy > 0)           { uint8_t *u = c - NEBULA_W; int e = *u + 90; *u = e > 255 ? 255 : (uint8_t)e; }
                if (yy < NEBULA_H - 1) { uint8_t *d = c + NEBULA_W; int e = *d + 90; *d = e > 255 ? 255 : (uint8_t)e; }
            }
        }
    }

    /* Colour scheme steps to a new hue family every ~15s (eased in, not an
     * abrupt cut), with only a slow drift in between — calmer than a constant
     * rainbow cycle. The golden-angle step keeps consecutive schemes well
     * separated. Intensity 0 stays black. */
    double target_hue = (double)((int)(now_sec() / 15.0)) * 2.39996;
    nebula_hue += (target_hue - nebula_hue) * 0.05;
    nebula_pal_phase += 0.0015 + energy * 0.010;   /* only a slow in-scheme drift */
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        double t = i / 255.0;
        /* Narrower hue spread across intensity (3.4 rad, not a full 2*pi) so a
         * scheme has a dominant colour family rather than being a full
         * rainbow — that's what makes the 15s hue step actually read as a
         * colour change instead of just rotating the same rainbow. */
        double base = nebula_hue + nebula_pal_phase + t * 3.4;
        int r = (int)((0.5 + 0.5 * sin(base))           * t * 255.0);
        int g = (int)((0.5 + 0.5 * sin(base + 2.09439)) * t * 255.0);
        int b = (int)((0.5 + 0.5 * sin(base + 4.18879)) * t * 255.0);
        pal[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    /* Upscale to the framebuffer (nearest), one 32-bit store per pixel. */
    for (int y = 0; y < fb->height; y++) {
        int base = nebula_ymap[y];
        uint32_t *row = (uint32_t *)(fb->back + (size_t)y * fb->stride);
        for (int x = 0; x < fb->width; x++)
            row[x] = pal[next[base + nebula_xmap[x]]];
    }

    /* A clean scope waveform painted straight onto the framebuffer AFTER the
     * plasma — it does NOT feed the feedback field, so it stays crisp and
     * always readable (the centered cover, blitted later by draw_now_playing,
     * masks the middle, leaving this line visible over the plasma to either
     * side). Continuous: each column fills the vertical span to the previous
     * sample so it reads as a line, not dots. */
    if (half > 0) {
        int   midy = fb->height / 2;
        double amp = fb->height * 0.30;
        int prevy  = midy;
        for (int x = 0; x < fb->width; x++) {
            int s  = samples[x * half / fb->width];
            int yy = midy + (int)((double)s / 32768.0 * amp);
            if (yy < 0) yy = 0;
            if (yy >= fb->height) yy = fb->height - 1;
            int y0 = yy < prevy ? yy : prevy;
            int y1 = yy < prevy ? prevy : yy;
            for (int y = y0; y <= y1; y++)
                *((uint32_t *)(fb->back + (size_t)y * fb->stride) + x) = 0x00E6F0FF;
            prevy = yy;
        }
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
#define TOASTY_MOON_W 120   /* height is derived from par_correction(), see toasty_draw()'s fb_blit */
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
    /* Was a fixed 0.6f (PAL's own pixel-aspect ratio, same fixed-factor bug
     * MiSTer-Toasty-Squadron's own moon once had) — derived from
     * par_correction() instead so sprites are also correct on NTSC. */
    int dh = (int)(size / par_correction(fb) + 0.5f);
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
    if (g_toasty_moon_on_screen) {
        /* TOASTY_MOON_H used to be a fixed 72 (=120*0.6, PAL's own pixel
         * aspect ratio, hand-tuned same as MiSTer-Toasty-Squadron's own
         * MOON_H once was) — derived from par_correction() instead so it's
         * also correct on NTSC's 240 lines. */
        int moon_h = (int)(TOASTY_MOON_W / par_correction(fb) + 0.5);
        fb_blit(fb, g_toasty_moon_px, g_toasty_moon_w, g_toasty_moon_h,
                (int)g_toasty_moon_x, (int)g_toasty_moon_y, TOASTY_MOON_W, moon_h, 255);
    }

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
        pthread_mutex_lock(&g_upd_mutex);
        UpdateState us = g_upd_state;
        char latest[32];
        strncpy(latest, g_upd_latest, sizeof(latest) - 1);
        latest[sizeof(latest) - 1] = '\0';
        pthread_mutex_unlock(&g_upd_mutex);

        pthread_mutex_lock(&g_inst_mutex);
        InstallState is = g_inst_state;
        pthread_mutex_unlock(&g_inst_mutex);

        /* Same bottom margin as everything else now (SAFE_Y_BOT == SAFE_Y) —
         * not the taller margin MiSTerDVD's own about screen used. */
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
            snprintf(upd, sizeof(upd), "%s available   B: install", latest);
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
        /* Solved from the space available down to the hint bar (title +
         * reason + the 4-line jellyfin.conf instructions below are fixed-
         * height text that doesn't shrink with resolution), capped at 110
         * (the original PAL-tuned size) so this is a no-op at 288 active
         * lines. Without it, this screen's text overlapped the hint row on
         * NTSC's 240 lines the same way draw_info's cast row once did. */
        int max_h = fb->height - 156;
        if (max_h > 110) max_h = 110;
        if (max_h < 50) max_h = 50;
        int img_h_box = max_h < img_h ? max_h : img_h;
        int img_w_box = (int)((double)img_w / img_h * img_h_box * par_correction(fb) + 0.5);
        if (img_w_box > max_w) { img_h_box = img_h_box * max_w / img_w_box; img_w_box = max_w; }
        blit_fit_centered(fb, img_px, img_w, img_h,
                           fb->width / 2, cur_y + img_h_box / 2, img_w_box, img_h_box, 255);
        cur_y += img_h_box + img_gap;
    }

    draw_text(fb, (fb->width - text_width(fb, title, ts)) / 2, cur_y, title, ts, COL_TITLE);
    cur_y += tch + lsp;
    draw_text(fb, (fb->width - text_width(fb, reason, s1)) / 2, cur_y, reason, s1, COL_ERR);
    cur_y += sch + lsp * 2;

    const char *l1 = "Create /media/fat/misterfin/jellyfin.conf with:";
    draw_text(fb, (fb->width - text_width(fb, l1, s1)) / 2, cur_y, l1, s1, COL_HINT);
    cur_y += sch + lsp;
    static const char *fields[] = { "server_url", "api_key", "username" };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        draw_text(fb, (fb->width - text_width(fb, fields[i], s1)) / 2, cur_y, fields[i], s1, COL_ITEM);
        cur_y += sch + lsp;
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

    if (!jf_quick_connect_enabled(&g_cfg)) { qc_set_state(QC_UNAVAILABLE); return NULL; }

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

    /* Laid out from the vertical centre outwards rather than from the top,
     * so it sits right at both PAL's 288 lines and NTSC's 240 without a
     * per-resolution case. */
    int cur_y = fb->height / 2 - 58;

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
 * screen (mirrors MiSTerDVD's own pdh=160/max_pw=200 proportions, scaled
 * down for this smaller corner panel). */
#define BROWSE_COVER_W 175
#define BROWSE_COVER_H 140

static uint8_t *g_browse_cover_px = NULL;
static int      g_browse_cover_w = 0, g_browse_cover_h = 0;
static char     g_browse_cover_item_id[JF_ID_LEN] = "";

/* Cache-file path for one item's cover on the SD card. Keyed by item id plus
 * a few chars of the image tag, so replacing the artwork server-side lands on
 * a new filename rather than serving stale art. */
static void cover_cache_path(const char *item_id, const char *tag, char *out, size_t outsz)
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
    snprintf(out, outsz, COVER_CACHE_DIR "/%s.img", safe);
}

/* Downloads one cover into the SD cache if not already there. Downloads to a
 * ".tmp" sibling IN THE CACHE DIR (so the rename into place stays within one
 * filesystem — a /tmp->/media/fat rename fails with EXDEV) and renames it, so
 * the main thread's browse_cover_load never reads a half-written file. */
static void cover_fetch_to_cache(const char *image_item_id, const char *tag,
                                  const char *item_id)
{
    if (!tag[0]) return;
    char cpath[160];
    cover_cache_path(item_id, tag, cpath, sizeof(cpath));
    struct stat st;
    if (stat(cpath, &st) == 0 && st.st_size > 0) return;   /* already cached */
    mkdir(COVER_CACHE_DIR, 0755);   /* ensure the dir exists for tmp + final */
    char tmp[176];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
    if (jf_download_item_image(&g_cfg, image_item_id, "Primary", tag, 180, tmp)) {
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
        (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_EPISODE ||
         it->type == JF_TYPE_SERIES || it->type == JF_TYPE_SEASON ||
         it->type == JF_TYPE_ARTIST || it->type == JF_TYPE_ALBUM || it->type == JF_TYPE_TRACK);
    return wants ? it->id : "";
}

/* Loads the selected row's cover into the top-right panel — from the SD cache
 * ONLY, never the network. A cached cover shows instantly (no freeze, no
 * blank); an as-yet-uncached one leaves the panel blank until the background
 * prefetch (cover_prefetch_thread) writes it to the card, at which point the
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

    char cpath[160];
    cover_cache_path(it->id, it->image_tag, cpath, sizeof(cpath));
    int w = 0, h = 0;
    uint8_t *px = load_image_keep(cpath, &w, &h);
    if (!px) return;   /* not cached yet — blank; prefetch fills it, next redraw loads it */
    g_browse_cover_px = px;
    g_browse_cover_w = w; g_browse_cover_h = h;
    strncpy(g_browse_cover_item_id, it->id, sizeof(g_browse_cover_item_id) - 1);
    g_browse_cover_item_id[sizeof(g_browse_cover_item_id) - 1] = '\0';
}

/* Background fill of the SD cover cache for the current list, so scrolling
 * reads covers off the card. Snapshots the item ids/tags (never touches
 * g_items off-thread) and stops early if the list changed under it (g_cover_gen). */
typedef struct {
    int gen, n;
    struct { char id[JF_ID_LEN], img_id[JF_ID_LEN], tag[JF_ID_LEN]; } items[JF_PAGE_SIZE];
} CoverPrefetch;

static void *cover_prefetch_thread(void *arg)
{
    CoverPrefetch *cp = (CoverPrefetch *)arg;
    for (int i = 0; i < cp->n; i++) {
        if (g_cover_gen != cp->gen) break;   /* navigated away — stop */
        cover_fetch_to_cache(cp->items[i].img_id, cp->items[i].tag, cp->items[i].id);
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
            (it->type == JF_TYPE_MOVIE || it->type == JF_TYPE_EPISODE ||
             it->type == JF_TYPE_SERIES || it->type == JF_TYPE_SEASON ||
             it->type == JF_TYPE_ARTIST || it->type == JF_TYPE_ALBUM || it->type == JF_TYPE_TRACK);
        if (!wants) continue;
        snprintf(cp->items[cp->n].id,     JF_ID_LEN, "%s", it->id);
        snprintf(cp->items[cp->n].img_id, JF_ID_LEN, "%s", it->image_item_id);
        snprintf(cp->items[cp->n].tag,    JF_ID_LEN, "%s", it->image_tag);
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
#define CAROUSEL_CARD_W 180   /* max text width before truncating */

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
        const char *ct = it->collection_type;
        char cbuf[32];
        if (view_is_synthetic(it))
            snprintf(cbuf, sizeof(cbuf), "%lld item%s", (long long)count, count == 1 ? "" : "s");
        else if (!strcmp(ct, "movies"))
            snprintf(cbuf, sizeof(cbuf), "%lld movie%s", (long long)count, count == 1 ? "" : "s");
        else if (!strcmp(ct, "tvshows"))
            snprintf(cbuf, sizeof(cbuf), "%lld series", (long long)count);
        else if (!strcmp(ct, "music"))
            snprintf(cbuf, sizeof(cbuf), "%lld album%s", (long long)count, count == 1 ? "" : "s");
        else
            snprintf(cbuf, sizeof(cbuf), "%lld item%s", (long long)count, count == 1 ? "" : "s");
        draw_text(fb, cx - text_width(fb, cbuf, 1) / 2, cy + 12, cbuf, 1, COL_HINT);
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
    /* Set only once grid_cache_populate() fully finishes — a slot's
     * view_id becomes visible to other threads' scans (under g_grid_mutex)
     * as soon as it's reserved, before population (which can take a while
     * and deliberately runs without the lock held) completes. Checked by
     * draw_grid_background so it never renders a slot mid-fill. */
    int      ready;
} GridLibCache;

static GridLibCache g_grid_cache[GRID_LIB_CACHE_MAX];
static int           g_grid_cache_n  = 0;    /* slots filled so far */
static int           g_grid_active   = -1;   /* index of the currently-shown library's slot, -1 = none */
/* Protects g_grid_cache/g_grid_cache_n/g_grid_active — grid_covers_sync()
 * (main thread, interactive) and grid_prefetch_thread() (background,
 * launched once from the home screen) both touch these. */
static pthread_mutex_t g_grid_mutex = PTHREAD_MUTEX_INITIALIZER;

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

/* Turns a Jellyfin GUID view_id into a safe filename — GUIDs are already
 * hex+dashes so this is just a defensive fallback, not real sanitizing. */
static void grid_cache_disk_path(const char *view_id, char *out, size_t outsz)
{
    char safe[JF_ID_LEN];
    size_t j = 0;
    for (size_t i = 0; view_id[i] && j < sizeof(safe) - 1; i++) {
        char c = view_id[i];
        safe[j++] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
    }
    safe[j] = '\0';
    snprintf(out, outsz, GRID_CACHE_DIR "/%s.dat", safe);
}

/* Persisted grid cache survives an app restart — without it, every
 * library's mosaic had to be re-downloaded/re-decoded from scratch on
 * every single launch even though nothing in the library had changed,
 * confirmed as a real lag spike by the user. Freshness check is a single
 * cheap jf_count_items() (Limit=0) call: if the library's total item count
 * still matches what it was when this was written, trust the cache as-is.
 * Doesn't catch a same-count swap (one item replaced by another), but
 * that's a rare edge case for what's purely decorative background art.
 * Pixels are stored raw/uncompressed rather than re-encoded to JPEG (this
 * build's stb_image.h is decode-only, no encoder) — at most ~480KB per
 * library (12 covers * ~100x100x4 bytes), trivial for an SD card. */
static int grid_cache_load_from_disk(GridLibCache *gc, const JfItem *view, const char *item_type)
{
    int64_t current_count = jf_count_items(&g_cfg, view->id, item_type);
    if (current_count < 0) return 0;

    char path[300];
    grid_cache_disk_path(view->id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int64_t cached_count = -1;
    int32_t n_covers = 0;
    if (fread(&cached_count, sizeof(cached_count), 1, f) != 1 ||
        fread(&n_covers, sizeof(n_covers), 1, f) != 1 ||
        cached_count != current_count || n_covers <= 0 || n_covers > GRID_FETCH_MAX) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < n_covers; i++) {
        int32_t w = 0, h = 0;
        size_t need;
        if (fread(&w, sizeof(w), 1, f) != 1 || fread(&h, sizeof(h), 1, f) != 1 ||
            /* Bounded individually before multiplying: size_t is 32-bit on
             * the ARM target, so w*h*4 can wrap and slip under the 4MB guard
             * on a corrupted cache file. */
            w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
            (need = (size_t)w * (size_t)h * 4) > 4 * 1024 * 1024) {
            fclose(f);
            for (int k = 0; k < gc->count; k++) { free(gc->px[k]); gc->px[k] = NULL; }
            gc->count = 0;
            return 0;
        }
        uint8_t *px = malloc(need);
        if (!px || fread(px, 1, need, f) != need) {
            free(px);
            fclose(f);
            for (int k = 0; k < gc->count; k++) { free(gc->px[k]); gc->px[k] = NULL; }
            gc->count = 0;
            return 0;
        }
        gc->px[gc->count] = px;
        gc->w[gc->count] = w;
        gc->h[gc->count] = h;
        gc->count++;
    }
    fclose(f);
    return gc->count > 0;
}

static void grid_cache_save_to_disk(const GridLibCache *gc, const char *view_id, int64_t count)
{
    if (gc->count <= 0) return;
    mkdir(GRID_CACHE_DIR, 0755);   /* ignore EEXIST/already-there */
    char path[300];
    grid_cache_disk_path(view_id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;

    int32_t n_covers = gc->count;
    fwrite(&count, sizeof(count), 1, f);
    fwrite(&n_covers, sizeof(n_covers), 1, f);
    for (int i = 0; i < gc->count; i++) {
        int32_t w = gc->w[i], h = gc->h[i];
        fwrite(&w, sizeof(w), 1, f);
        fwrite(&h, sizeof(h), 1, f);
        fwrite(gc->px[i], 1, (size_t)w * (size_t)h * 4, f);
    }
    fclose(f);
}

/* Fills a freshly-reserved (memset to 0, view_id already set) cache slot —
 * disk cache first (grid_cache_load_from_disk), falling back to sequential
 * downloads+decodes (up to GRID_FETCH_MAX) otherwise. Shared by the
 * interactive path (grid_covers_sync, called from the main thread when the
 * user navigates to a library) and grid_prefetch_thread (background,
 * silent). dest_path is caller-owned scratch space (POSTER_TMP for the
 * main thread, POSTER_TMP_BG for the background thread) so the two never
 * clobber each other's in-flight download; fb_show_ui is NULL for the
 * silent background path (no spinner/fb_flip — those must stay main-
 * thread-only, same reasoning as every other "don't touch the framebuffer
 * off-thread" rule already in this codebase). Deliberately NOT called
 * under g_grid_mutex — this does slow network I/O, and the slot it's
 * writing into isn't visible to any reader until the caller marks it
 * active/counted, so nothing needs the lock held here. */
static void grid_cache_populate(GridLibCache *gc, const JfItem *view,
                                 const char *dest_path, FBDev *fb_show_ui)
{
    const char *item_type = collection_item_type(view->collection_type);
    JfItem grid_items[GRID_FETCH_MAX];
    int n;

    if (view_is_synthetic(view)) {
        /* No ParentId to list under — the covers are just the row's own
         * items. Deliberately not disk-cached either: the whole point of
         * these rows is that they change as things get watched, so a cache
         * keyed on a total that barely moves would show stale art for ages
         * (see grid_cache_load_from_disk's own staleness check). They're only
         * ever a couple of items, so re-fetching is cheap. */
        n = view_is_resume(view)
              ? jf_list_resume(&g_cfg, grid_items, GRID_FETCH_MAX, NULL)
              : jf_list_nextup(&g_cfg, grid_items, GRID_FETCH_MAX, NULL);
    } else {
        if (grid_cache_load_from_disk(gc, view, item_type)) {
            grid_cell_order_shuffle(gc);
            /* Release: everything written above must be visible to any
             * thread that observes ready == 1. See the acquire in
             * draw_grid_background. */
            __atomic_store_n(&gc->ready, 1, __ATOMIC_RELEASE);
            return;
        }
        n = jf_list_items_recursive(&g_cfg, view->id, item_type, grid_items, GRID_FETCH_MAX);
    }

    int spinner_frame = 0;
    for (int i = 0; i < n && gc->count < GRID_FETCH_MAX; i++) {
        if (!grid_items[i].image_tag[0]) continue;
        if (fb_show_ui) { draw_spinner_frame(fb_show_ui, spinner_frame++); fb_flip(fb_show_ui); }
        if (jf_download_item_image(&g_cfg, grid_items[i].image_item_id, "Primary",
                                    grid_items[i].image_tag, 100, dest_path)) {
            uint8_t *px = load_image_tmp(dest_path, &gc->w[gc->count], &gc->h[gc->count]);
            if (px) gc->px[gc->count++] = px;
        }
    }
    grid_cell_order_shuffle(gc);
    if (!view_is_synthetic(view)) {
        int64_t count = jf_count_items(&g_cfg, view->id, item_type);
        if (count >= 0) grid_cache_save_to_disk(gc, view->id, count);
    }
    __atomic_store_n(&gc->ready, 1, __ATOMIC_RELEASE);
}

/* Already-cached libraries (checked here under g_grid_mutex) return
 * immediately — just a linear scan over however many are cached, at most
 * GRID_LIB_CACHE_MAX. A never-seen library reserves its slot under the
 * lock (cheap) then populates it lock-free (grid_cache_populate does the
 * slow network I/O) before marking it active. */
static void grid_covers_sync(FBDev *fb, const JfItem *view)
{
    pthread_mutex_lock(&g_grid_mutex);
    for (int i = 0; i < g_grid_cache_n; i++) {
        if (!strcmp(g_grid_cache[i].view_id, view->id)) {
            g_grid_active = i;
            pthread_mutex_unlock(&g_grid_mutex);
            return;
        }
    }
    if (g_grid_cache_n >= GRID_LIB_CACHE_MAX) {
        g_grid_active = -1;
        pthread_mutex_unlock(&g_grid_mutex);
        return;
    }
    int slot = g_grid_cache_n++;
    GridLibCache *gc = &g_grid_cache[slot];
    memset(gc, 0, sizeof(*gc));
    strncpy(gc->view_id, view->id, sizeof(gc->view_id) - 1);
    pthread_mutex_unlock(&g_grid_mutex);

    grid_cache_populate(gc, view, POSTER_TMP, fb);

    pthread_mutex_lock(&g_grid_mutex);
    g_grid_active = slot;
    pthread_mutex_unlock(&g_grid_mutex);
}

/* Runs once, kicked off (detached) right after the home screen first
 * loads. Silently walks every library the user has and pre-populates any
 * that the interactive path hasn't already claimed, so switching to a
 * library the user hasn't visited yet in this session still shows its
 * mosaic immediately instead of a blank/dim background while it fetches.
 * Uses its own download scratch path and never touches fb — see
 * grid_cache_populate's own comment for why. */
static void *grid_prefetch_thread(void *arg)
{
    (void)arg;
    JfItem views[GRID_LIB_CACHE_MAX];
    int n = jf_list_views(&g_cfg, views, GRID_LIB_CACHE_MAX);

    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&g_grid_mutex);
        int already = 0;
        for (int j = 0; j < g_grid_cache_n; j++)
            if (!strcmp(g_grid_cache[j].view_id, views[i].id)) { already = 1; break; }
        int slot = -1;
        if (!already && g_grid_cache_n < GRID_LIB_CACHE_MAX) {
            slot = g_grid_cache_n++;
            GridLibCache *gc = &g_grid_cache[slot];
            memset(gc, 0, sizeof(*gc));
            strncpy(gc->view_id, views[i].id, sizeof(gc->view_id) - 1);
        }
        pthread_mutex_unlock(&g_grid_mutex);
        if (slot < 0) continue;

        grid_cache_populate(&g_grid_cache[slot], &views[i], POSTER_TMP_BG, NULL);
    }
    return NULL;
}

static void draw_grid_background(FBDev *fb)
{
    if (g_grid_active < 0) return;
    GridLibCache *gc = &g_grid_cache[g_grid_active];
    if (!__atomic_load_n(&gc->ready, __ATOMIC_ACQUIRE) || gc->count == 0) return;
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
        draw_text(fb, (fb->width - text_width(fb, msg, 1))/2, fb->height/2, msg, 1, COL_HINT);
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

    browse_cover_load();

    fb_clear(fb);

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
        int dw = (int)((double)g_browse_cover_w / g_browse_cover_h * dh * par_correction(fb) + 0.5);
        if (dw > BROWSE_COVER_W) dh = dh * BROWSE_COVER_W / dw;
        int cy = cover_panel_y + dh / 2;
        blit_fit_centered(fb, g_browse_cover_px, g_browse_cover_w, g_browse_cover_h,
                           cx, cy, BROWSE_COVER_W, BROWSE_COVER_H, 255);
    }

    int visible = visible_rows(fb);
    int end = g_scroll + visible;
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
        truncate_to_width(fb, line1, 1, row_max_w);

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
        truncate_to_width(fb, line2, 1, row_max_w);

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

/* Blits src into a max_w x max_h box, preserving its own real-world aspect
 * ratio and centering it (letterboxing/pillarboxing as needed — handles a
 * landscape screen-grab used as a poster fine, same as a normal portrait
 * one). Used for the logo and any poster/cover, which — unlike the
 * full-bleed backdrop crop-fill — must never look distorted.
 *
 * par_correction() is MiSTerDVD's proven correction for this platform's
 * non-square pixels, generalized to the live framebuffer size: our buffer
 * is 640 wide feeding a 4:3 CRT through a narrower final PAL/NTSC DDR
 * resolution, so plain w/h aspect math alone renders posters visibly too
 * narrow ("elongated") on the real screen — was hardcoded to 5/3 (correct
 * only at 288 active lines), now derived so it's also correct at NTSC's
 * 240. */
static void blit_fit_centered(FBDev *fb, const uint8_t *src, int sw, int sh,
                               int cx, int cy, int max_w, int max_h, uint8_t alpha)
{
    if (!src || sw <= 0 || sh <= 0) return;
    int dh = max_h;
    int dw = (int)((double)sw / sh * dh * par_correction(fb) + 0.5);
    if (dw > max_w) { dh = dh * max_w / dw; dw = max_w; }
    fb_blit(fb, src, sw, sh, cx - dw / 2, cy - dh / 2, dw, dh, alpha);
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
    memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);

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
    const char *hint = (g_info_item.sub_count > 0 || g_info_item.audio_count > 1)
        ? "B:resume  A:stop  L/R:vsync  SELECT:tracks"
        : "B:resume  A:stop  L/R:vsync";
    draw_text(fb, (fb->width - text_width(fb, hint, 1)) / 2,
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
#define SUBMENU_TAB_AUDIO 0
#define SUBMENU_TAB_SUBS  1

static int    g_submenu_visible    = 0;
static int    g_submenu_tab        = SUBMENU_TAB_SUBS;
static int    g_submenu_sub_sel    = 0;   /* 0 = Off, i+1 = g_info_item.subs[i] */
static int    g_submenu_audio_sel  = 0;   /* index into g_info_item.audio[] */
static int    g_submenu_was_paused = 0;   /* pause state before the menu opened, to restore on close */
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
    return g_info_item.sub_count + 1;        /* + "Off" */
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
    if (g_submenu_bg) memcpy(g_submenu_bg, fb->mem, sz);
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

    /* Open on whichever tab is actually useful. Subtitles stay the default
     * (that's what SELECT has always opened), but on a title with no subtitle
     * tracks at all and a real choice of audio, opening on an empty list
     * would look broken. */
    if (g_info_item.sub_count == 0 && g_info_item.audio_count > 1)
        g_submenu_tab = SUBMENU_TAB_AUDIO;

    g_submenu_was_paused = g_paused;
    g_submenu_opened_at  = now_sec();
    if (!g_paused) player_pause_toggle();
    submenu_bg_capture(fb);
    memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);
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

static void draw_submenu(FBDev *fb)
{
    /* Start from the captured backdrop every time — see g_submenu_bg. */
    submenu_bg_restore(fb);

    int is_audio = (g_submenu_tab == SUBMENU_TAB_AUDIO);
    int n_opts   = submenu_option_count();
    int sel      = is_audio ? g_submenu_audio_sel : g_submenu_sub_sel;

    /* At least one row of vertical space even when a tab has no tracks, so
     * the "none" message below has somewhere to go. */
    int n_rows   = n_opts > 0 ? n_opts : 1;
    /* The sync readout is subtitle-only — it adjusts subtitle timing, and
     * there's no audio equivalent worth the row. */
    int extra_rows = is_audio ? 0 : 1;

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
    static int submenu_scroll[2] = { 0, 0 };
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

    /* Tab header — the inactive tab stays visible (dimmed) rather than being
     * hidden, so there's something on screen telling you the other one is
     * there and that L/R reaches it. */
    {
        const char *audio_label = "AUDIO";
        const char *subs_label  = "SUBTITLES";
        int gap = 3 * 8;
        int audio_x = list_x;
        int subs_x  = audio_x + text_width(fb, audio_label, 1) + gap;
        if (is_audio) {
            draw_text(fb, audio_x, list_y, audio_label, 1, COL_TITLE);
            draw_text(fb, subs_x,  list_y, subs_label,  1, COL_HINT);
        } else {
            draw_text(fb, audio_x, list_y, audio_label, 1, COL_HINT);
            draw_text(fb, subs_x,  list_y, subs_label,  1, COL_TITLE);
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

    if (!is_audio) {
        char syncline[32];
        snprintf(syncline, sizeof(syncline), "Sync: %+.1fs", g_sub_delay_extra);
        draw_text(fb, list_x, list_y, syncline, 1, COL_ITEM);
        list_y += 15;
    }
    list_y += 8;   /* gap before the hint block */

    const char *hint1 = is_audio ? "UP/DOWN: select audio track"
                                  : "UP/DOWN: select subtitle";
    const char *hint2 = is_audio ? "L/R: switch tab"
                                  : "L/R: switch tab   LEFT/RIGHT: sync";
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

    submenu_close();

    /* An audio change can only be applied by rebuilding the stream URL, so it
     * subsumes any subtitle change made in the same visit — both get baked
     * into the one restart rather than restarting twice. */
    if (new_audio != g_current_audio_index) {
        double pos = play_position();
        g_current_audio_index = new_audio;

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

    char delay_arg[16];
    snprintf(delay_arg, sizeof(delay_arg), "%.2f", AUDIO_DELAY_SEC);

    g_play_offset     = offset_secs > 0.0 ? offset_secs : 0.0;
    g_play_start_wall = now_sec();
    g_paused          = 0;
    g_last_progress_report = now_sec();
    jf_report_start(&g_cfg, item_id, g_play_session_id, start_ticks);

    char vf_arg[96];
    if (fb->height == 288) {
        /* PAL — byte-for-byte the original, long-proven chain. Deliberately
         * NOT touched by the NTSC comb fix below: PAL's width-first
         * "scale=640:-1" mismatch is real (confirmed via the same PAR math
         * used for NTSC) but mild enough to have never shown a visible
         * artifact, and there's no reason to risk regressing something
         * that's worked for the platform's entire history over a
         * theoretical aspect-precision improvement it doesn't need. */
        snprintf(vf_arg, sizeof(vf_arg), "scale=%d:-1,expand=%d:%d:-1:-1:1,dsize=%d:%d",
                 fb->width, fb->width, fb->height, fb->width, fb->height);
    } else {
        /* NTSC (and any other non-288 height) — confirmed on hardware that
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
        double dar = g_info_item.source_aspect;
        if (dar <= 0.0 && g_info_item.source_width > 0 && g_info_item.source_height > 0)
            dar = (double)g_info_item.source_width / (double)g_info_item.source_height;
        if (dar <= 0.0) dar = 16.0 / 9.0;

        int target_h = (int)(4.0 * fb->height / (3.0 * dar) + 0.5);
        target_h &= ~1;                        /* even, required for yuv420p chroma subsampling */
        if (target_h < 2) target_h = 2;
        if (target_h > fb->height) target_h = fb->height;

        snprintf(vf_arg, sizeof(vf_arg), "scale=%d:%d,expand=%d:%d:-1:-1:1,dsize=%d:%d",
                 fb->width, target_h, fb->width, fb->height, fb->width, fb->height);
    }

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
               "-vf", vf_arg,
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
               /* The sanitized .srt we write is UTF-8 (jf_text_to_display now
                * keeps Latin-1 accents rather than folding them). -utf8 tells
                * mplayer to decode it as UTF-8 and look each code point up in
                * the subtitle font, which now carries U+00A0-U+00FF glyphs
                * (see gen_subfont.py) — so accented subtitles render instead
                * of dropping the accented characters. */
               "-utf8",
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
    g_np_title_shown_until = now_sec() + 5.0;   /* flash the new title in immersive mode */

    unlink(AF_EXPORT_PATH);   /* don't let the visualizer read a stale previous track's data */

    jf_make_play_session_id(g_play_session_id, sizeof(g_play_session_id));
    char url[700];
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

    /* Live PCM for the audio-reactive effect (Nebula) and the VU meters
     * below — read once, up here, so the background pass can use it too.
     * Not read while paused: decode has stopped, so the export file would
     * just return stale pre-pause samples (a frozen meter, per user
     * feedback) instead of settling to empty. */
    int16_t af_buf[4096];
    int af_n = g_paused ? 0 : read_af_samples(af_buf, 4096);

    int nebula = (g_now_playing_bg == NOW_PLAYING_BG_NEBULA);
    /* Both Nebula and Toasty Squadron use the immersive layout — just the
     * enlarged centered cover over the effect plus the bottom hint bar
     * (Toasty's sprites still fly over the top, see draw_toasty_fg below). */
    int immersive = nebula || (g_now_playing_bg == 3);

    if      (g_now_playing_bg == 1) draw_rain(fb);
    else if (nebula)                 draw_nebula(fb, af_buf, af_n);
    else if (g_now_playing_bg == 3) { draw_toasty_bg(fb); draw_now_playing_gradient(fb); }
    else                            draw_starfield(fb);

    /* The brief "which background" label on SELECT is shown in every mode
     * (that's how the cycle stays legible); the clock and PAUSED marker are
     * hidden in the immersive modes, which keep only the cover + hints. */
    if (now_sec() < g_now_playing_bg_shown_until)
        draw_text(fb, SAFE_X, SAFE_Y, NOW_PLAYING_BG_NAMES[g_now_playing_bg], 1, COL_HINT);
    /* In the immersive modes the title is otherwise hidden, so flash it at the
     * top for a few seconds on each track change (see g_np_title_shown_until,
     * set in play_audio). Nudged down a line if the bg-name label happens to
     * be up at the same moment so the two don't overlap. */
    if (immersive && now_sec() < g_np_title_shown_until) {
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

    if (immersive) {
        /* Immersive mode: enlarged cover centered in the screen over the
         * effect, and nothing else but the shared hint bar below. Seeking
         * still works (L/R input untouched) — only its timeline readout is
         * hidden. */
        if (g_nowplaying_cover_px) {
            int top    = (fb->height == 288) ? SAFE_Y : 2;
            int bottom = fb->height - 8 - SAFE_Y_BOT - 6;
            int box_h  = bottom - top;
            int box_w  = (fb->height == 288) ? box_h : (int)(box_h * par_correction(fb) + 0.5);
            blit_fit_centered(fb, g_nowplaying_cover_px, g_nowplaying_cover_w, g_nowplaying_cover_h,
                               fb->width / 2, (top + bottom) / 2, box_w, box_h, 255);
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
    if (g_now_playing_bg == 3) draw_toasty_fg(fb);

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
    push_frame(FRAME_VIEWS, "MiSTerFin", NULL, NULL, NULL);
}

/* Kicks off the background cover-grid prefetch. Deliberately not started
 * unconditionally at launch: it needs a resolved user and a working
 * credential, and starting it from the setup or Quick Connect screens meant
 * firing a /UserViews with an empty userId and no token — harmless against a
 * permissive server, a guaranteed 401 against a real one, and a confusing
 * entry in the log for anyone debugging why their client won't connect.
 * Called from both paths that reach a signed-in library. */
static void start_grid_prefetch(void)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, grid_prefetch_thread, NULL);
    pthread_attr_destroy(&attr);
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

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--capture-about") == 0)
        return run_capture_about(argc > 2 ? atoi(argv[2]) : 60);
    if (argc > 1 && strcmp(argv[1], "--preview-browse") == 0)
        return run_preview_browse(argc > 2 ? atoi(argv[2]) : -1,
                                   argc > 3 && strcmp(argv[3], "list") == 0);
    if (argc > 1 && strcmp(argv[1], "--preview-submenu") == 0)
        return run_preview_submenu(argc > 2 ? argv[2] : "",
                                    (argc > 3 && strcmp(argv[3], "audio") == 0)
                                        ? SUBMENU_TAB_AUDIO : SUBMENU_TAB_SUBS);

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
    g_headless = fb.headless;   /* see g_headless' own comment */
    /* SAFE_Y as a plain pixel count made the top/bottom margin look
     * noticeably BIGGER than the left/right margin on real hardware, even
     * though 20 < 24 — because our pixels aren't square. Physically,
     * SAFE_Y rows are worth more screen distance than SAFE_X columns by
     * exactly par_correction()'s factor, so dividing by it here equalizes
     * the four margins in real physical terms instead of raw pixel count.
     * At fb->height=288 this comes out to 24/1.667≈14 (was a flat 20) —
     * confirmed as a real improvement there too, not NTSC-only. */
    SAFE_Y = (int)(SAFE_X / par_correction(&fb) + 0.5);
    memcpy(fb.mem, fb.back, (size_t)fb.stride * fb.height);

    cursor_hide();
    input_open();
    input_drain();

    /* Enable vsync by default — mplayer's patched vo_fbdev checks this file
     * each frame (see VSYNC_FLAG comment above). */
    { int vf = open(VSYNC_FLAG, O_WRONLY|O_CREAT|O_TRUNC, 0644); if (vf >= 0) close(vf); }

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
        draw_setup_screen(&fb, g_setup_reason);
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
        snprintf(g_setup_reason, sizeof(g_setup_reason),
                 resolved == -1 ? "Can't connect to server (check server URL)"
                                : "Username not found on server (check spelling)");
        draw_setup_screen(&fb, g_setup_reason);
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
    if (state == STATE_BROWSE) start_grid_prefetch();

    int playing = 0;
    int spinner_frame_ctr = 0;
    int about_visible = 0;
    double last_about_press = 0.0;
    if (state == STATE_BROWSE) draw_browse(&fb);

    double last_input_rescan = 0.0;
    while (g_running) {
        int inp = input_poll();
        /* Must be called every tick (it drives the repeat timers), but only
         * OR'd in by the screens that want held-to-scroll — see
         * input_repeat()'s own comment for why it isn't just part of inp. */
        int nav_repeat = input_repeat();
        double loop_now = now_sec();

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
                if (inp & INP_A) about_start_install();
                draw_about(&fb);   /* redraw every frame to pick up update state */
            }
            usleep(16000);
            continue;
        }

        if (g_submenu_visible) {
            static double last_nav_press = 0.0;
            int n_opts   = submenu_option_count();
            int is_audio = (g_submenu_tab == SUBMENU_TAB_AUDIO);
            int *sel     = is_audio ? &g_submenu_audio_sel : &g_submenu_sub_sel;

            /* Shoulder buttons switch tabs, leaving LEFT/RIGHT free for
             * subtitle sync (which needs to stay a fine repeated nudge, and
             * has no audio equivalent to share the keys with). */
            if (inp & INP_L) submenu_switch_tab(SUBMENU_TAB_AUDIO);
            if (inp & INP_R) submenu_switch_tab(SUBMENU_TAB_SUBS);

            int menu_inp = inp | nav_repeat;   /* held UP/DOWN walks the list, held LEFT/RIGHT keeps nudging sync */
            if ((menu_inp & (INP_UP | INP_DOWN | INP_LEFT | INP_RIGHT)) &&
                loop_now - last_nav_press > 0.15) {
                last_nav_press = loop_now;
                if (menu_inp & INP_UP)    { if (*sel > 0) (*sel)--; }
                if (menu_inp & INP_DOWN)  { if (*sel < n_opts - 1) (*sel)++; }
                /* Live-tunable on top of the fixed baseline (AUDIO_DELAY_SEC
                 * + SUBTITLE_SYNC_FUDGE_SEC + g_play_offset) — for whatever
                 * that fixed default doesn't cover on a specific subtitle
                 * file. Applies immediately if a subtitle is already
                 * loaded. Subtitle tab only: there's nothing for it to mean
                 * on the audio tab, and silently changing subtitle timing
                 * from a screen not showing it would be a surprise. */
                if (!is_audio && (menu_inp & INP_LEFT))  { g_sub_delay_extra -= 0.1; sub_delay_send(); }
                if (!is_audio && (menu_inp & INP_RIGHT)) { g_sub_delay_extra += 0.1; sub_delay_send(); }
            }
            if (inp & INP_A) { submenu_confirm(&fb); input_drain(); continue; }
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
                submenu_close(); input_drain(); continue;
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
            draw_submenu(&fb);
            usleep(16000);
            continue;
        }

        switch (state) {

        case STATE_CONFIG_ERROR:
            if (inp & INP_B) { g_running = 0; break; }
            draw_setup_screen(&fb, g_setup_reason);   /* redrawn every frame for the starfield */
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
                start_grid_prefetch();   /* skipped at launch — see its comment */
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
            if (inp & INP_A) { g_running = 0; break; }

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
                        push_frame(FRAME_RESUME, "Continue Watching", NULL, NULL, NULL);
                    else if (view_is_nextup(it))
                        push_frame(FRAME_NEXTUP, "Next Up", NULL, NULL, NULL);
                    else
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

        case STATE_PLAYING:
            if (!player_running()) {
                double pos = play_position();
                int watched = playback_watched(g_info_item.runtime_ticks, pos);
                jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                                   (int64_t)(pos * 10000000.0), watched);
                g_info_item.played = watched;
                g_info_item.resume_ticks = watched ? 0 : (int64_t)(pos * 10000000.0);
                playing = 0;
                state = STATE_BROWSE;
                refetch_frame_keep_selection(&fb);
                draw_browse(&fb);
            } else if (inp & INP_B) {
                double pos = play_position();
                int watched = playback_watched(g_info_item.runtime_ticks, pos);
                jf_report_stopped(&g_cfg, g_info_item.id, g_play_session_id,
                                   (int64_t)(pos * 10000000.0), watched);
                g_info_item.played = watched;
                g_info_item.resume_ticks = watched ? 0 : (int64_t)(pos * 10000000.0);
                player_stop();
                playing = 0;
                state = STATE_BROWSE;
                refetch_frame_keep_selection(&fb);
                draw_browse(&fb);
            } else if (g_paused) {
                if (inp & INP_SELECT) { submenu_open(&fb); draw_submenu(&fb); input_drain(); continue; }
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
                if (inp & INP_SELECT) { submenu_open(&fb); draw_submenu(&fb); input_drain(); continue; }
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
                    report_progress_async(g_info_item.id, g_play_session_id,
                                          (int64_t)(play_position() * 10000000.0), 0);
                }
            }
            break;

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
                g_now_playing_bg = (g_now_playing_bg + 1) % NOW_PLAYING_BG_COUNT;
                g_now_playing_bg_shown_until = now_sec() + 1.5;
                if (g_now_playing_bg == 3 && !g_toasty_loaded) {
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

    /* The Quick Connect thread can still be polling when the user quits. It
     * checks g_running in 100ms slices, so this waits well under a second —
     * but without it, main() returning would run glibc's exit-time FILE
     * cleanup underneath a thread that is mid-read on a curl pipe. */
    if (qc_running) { pthread_join(qc_tid, NULL); qc_running = 0; }

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
