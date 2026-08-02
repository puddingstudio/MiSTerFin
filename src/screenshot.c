/* Screenshot (SELECT+START) — see screenshot.h for the interface and why
 * this exists instead of MiSTer's own hotkey. */

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#include "screenshot.h"
#include "draw.h"

#define SCREENSHOT_DIR "/media/fat/screenshots/MiSTerFin"

/* BMP wants little-endian fields regardless of host order; the MiSTer ARM
 * target is little-endian, but write explicitly rather than assume. */
static void bmp_put_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void bmp_put_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

/* Writes a real-aspect screenshot: fb->mem is BGRX8888 at the LOGICAL
 * width/height (already halved for line_double — see fb.h), which for a
 * progressive PAL/NTSC buffer is not square-pixel 4:3 the way it looks on
 * the actual CRT. par_correction() is the same factor cover art is scaled
 * by everywhere else in this app; applying it here to the OUTPUT HEIGHT
 * instead (nearest-neighbor row repeat, keeping every source column) turns
 * 640x288 into a real 640x480 — genuine 4:3 — without resampling the
 * horizontal detail that's actually there. Same math naturally also
 * produces a correct 4:3 frame for NTSC's 640x240 and for the interlaced
 * core's full-frame modes, since fb->height there is already the logical
 * (halved) value par_correction expects. */
void screenshot_take(FBDev *fb)
{
    mkdir("/media/fat/screenshots", 0755);   /* usually already exists — MiSTer's own dir */
    mkdir(SCREENSHOT_DIR, 0755);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char path[128];
    snprintf(path, sizeof(path), SCREENSHOT_DIR "/%04d%02d%02d_%02d%02d%02d.bmp",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    int out_w = fb->width;
    int out_h = (int)(fb->height * par_correction(fb) + 0.5);
    if (out_h < fb->height) out_h = fb->height;   /* never shrink — see the comment above */

    int row_bytes = out_w * 3;
    int row_pad   = (4 - (row_bytes % 4)) % 4;
    uint32_t pixel_bytes = (uint32_t)(row_bytes + row_pad) * (uint32_t)out_h;

    FILE *f = fopen(path, "wb");
    if (!f) return;

    /* BITMAPFILEHEADER (14 bytes) */
    fputc('B', f); fputc('M', f);
    bmp_put_u32(f, 14 + 40 + pixel_bytes);
    bmp_put_u32(f, 0);
    bmp_put_u32(f, 14 + 40);
    /* BITMAPINFOHEADER (40 bytes) */
    bmp_put_u32(f, 40);
    bmp_put_u32(f, (uint32_t)out_w);
    bmp_put_u32(f, (uint32_t)out_h);
    bmp_put_u16(f, 1);          /* planes */
    bmp_put_u16(f, 24);         /* bpp */
    bmp_put_u32(f, 0);          /* no compression */
    bmp_put_u32(f, pixel_bytes);
    bmp_put_u32(f, 2835);       /* ~72 DPI, cosmetic only */
    bmp_put_u32(f, 2835);
    bmp_put_u32(f, 0);
    bmp_put_u32(f, 0);

    /* BMP rows are stored bottom-up. Source row = nearest-neighbor mapping
     * from the stretched output row back to the fb->height-tall source. */
    static const uint8_t pad_bytes[3] = {0, 0, 0};
    for (int oy = out_h - 1; oy >= 0; oy--) {
        int sy = oy * fb->height / out_h;
        if (sy >= fb->height) sy = fb->height - 1;
        const uint8_t *row = fb_mem_row(fb, sy);
        for (int x = 0; x < out_w; x++) {
            /* BGRX -> BMP's BGR is a same-order truncation, no conversion. */
            fwrite(row + (size_t)x * 4, 1, 3, f);
        }
        if (row_pad) fwrite(pad_bytes, 1, (size_t)row_pad, f);
    }
    fclose(f);

    /* Confirmation flash, works from any screen (browse, playback, music):
     * fb_sync_back pulls whatever's actually on screen into fb->back first
     * — essential during video, where mplayer writes fb->mem directly and
     * fb->back would otherwise be stale — then this draws on top of that
     * and flips once, the same compose-over-live-video approach the pause/
     * submenu overlays already use. Left on screen instead of restored
     * (the next real redraw, on the next input, replaces it naturally) —
     * same tradeoff spinner_show()'s blocking wait already makes.
     *
     * Fixed offset from the bottom edge rather than main.c's SAFE_Y_BOT:
     * this module has no other dependency on main.c's app-state globals,
     * and a mutable safe-area margin isn't worth introducing one for. */
    fb_sync_back(fb);
    const char *msg = "Screenshot saved";
    int tw = text_width(fb, msg, 1);
    int tx = (fb->width - tw) / 2, ty = fb->height - 24;
    fb_fill_rect_alpha(fb, tx - 8, ty - 4, tw + 16, 8 + 8, 0, 0, 0, 200);
    draw_text(fb, tx, ty, msg, 1, 80, 220, 120);
    fb_flip(fb);
}
