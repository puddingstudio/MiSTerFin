#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;
    uint8_t *mem;       /* mmap'd framebuffer, BGRX8888 (phys_height rows) */
    uint8_t *back;      /* off-screen back-buffer in RAM (height rows) */
    int      width, height;
    int      stride;    /* bytes per row */
    int      n_pages;   /* 1 or 2 (double-buffered fbdev) */
    size_t   mmap_size; /* total mmap'd bytes (stride * phys_height) */
    int      headless;  /* 1 = plain malloc'd buffers, no real fbdev (see fb_open) */
    /* Interlaced full-frame modes (the standalone interlaced menu core scans
     * out a real 576/480-line raster) report a framebuffer exactly DOUBLE
     * the 288/240-line layouts every piece of UI drawing code is tuned for.
     * Rather than teach the whole UI a second geometry, fb_open halves
     * `height` (the LOGICAL height all drawing code sees) and fb_flip writes
     * each logical back-buffer line twice — pixel-for-pixel the familiar
     * layout, drawn by the full interlaced raster. `phys_height` keeps the
     * real line count for the parts that genuinely want it (mplayer opens
     * /dev/fb0 itself and sees the physical size, so play()'s -vf chain must
     * be built from phys_height, letting video use the full vertical
     * resolution the UI deliberately doesn't). phys_height == height when
     * line_double == 0. */
    int      phys_height;
    int      line_double;
} FBDev;

/* Opens the real fbdev at `path`, UNLESS the MISTERFIN_FB environment
 * variable is set — in which case it fabricates an equivalent FBDev backed
 * by plain malloc'd buffers instead (see fb_open's own comment). Format is
 * "WxH", e.g. MISTERFIN_FB=640x288 for PAL, 640x240 for NTSC. */
int  fb_open(FBDev *fb, const char *path);
void fb_close(FBDev *fb);
void fb_clear(FBDev *fb);   /* clear back-buffer to black */
void fb_flip(FBDev *fb);    /* memcpy back->framebuffer (doubling lines if line_double) */

/* The mem->back direction of fb_flip: copies what is actually on screen
 * into the logical back buffer (downsampling every other line when
 * line_double). mplayer writes video frames straight to fb->mem, so
 * fb->back goes stale during playback — call this before compositing any
 * of our own overlays (pause screen, submenu, seek spinner) on top of a
 * live video frame. */
void fb_sync_back(FBDev *fb);

/* Pointer to the on-screen (fb->mem) pixel row for LOGICAL row y — maps
 * through the line doubling, so probing code can compare what it drew
 * against what is actually displayed without knowing the physical layout. */
uint8_t *fb_mem_row(FBDev *fb, int y);

/* Repoints the MiSTer scaler at framebuffer page 0 (the Linux fb this app
 * draws) or page 1 (the video player's flip page) — latched by the FPGA at
 * vsync, so the switch is atomic/tear-free. ONLY safe while Main_MiSTer is
 * SIGSTOPped; see pageflip_begin()/end() in main.c and the PF_* block in
 * docker/vo_fbdev.c. No-op when fb_open ran headless. */
void fb_page_flip(int page);

/* While on, every fb_flip() re-asserts page 0 after its copy — used while
 * the video player owns the display so app overlays (pause, submenu,
 * spinner) can't be flipped away by a late in-flight video frame. */
void fb_set_reflip(int on);

/* Blocks until the next real vblank (ioctl FBIO_WAITFORVSYNC on fb->fd) —
 * the same mechanism mplayer's patched vo_fbdev uses for its own frame
 * writes. Use to pace a read-side loop (e.g. sampling fb->mem for a DDR
 * passthrough) to actual display refresh timing instead of a blind
 * software timer. */
void fb_wait_vsync(FBDev *fb);

/* Nearest-neighbor scale RGBA8888 src onto back-buffer with alpha blend. */
void fb_blit(FBDev *fb,
             const uint8_t *pixels, int sw, int sh,
             int dx, int dy, int dw, int dh,
             uint8_t layer_alpha);

/* Same nearest-neighbor scaling as fb_blit, but no alpha blend at all —
 * every destination pixel is simply overwritten with the sampled source
 * RGB. Only correct where the source is known fully opaque AND any desired
 * dimming was already baked into its pixels ahead of time (see the grid
 * mosaic background, which pre-scales covers by GRID_ALPHA once at load
 * instead of blending against the always-black cleared backdrop on every
 * single redraw — confirmed via DEBUGLOG timing as this app's biggest
 * single CPU cost). Do not use this on anything that still needs real
 * per-pixel alpha or a variable layer_alpha. */
void fb_blit_opaque(FBDev *fb,
                     const uint8_t *pixels, int sw, int sh,
                     int dx, int dy, int dw, int dh);


/* Fill a rectangle in the back-buffer with a solid color + alpha blend. */
void fb_fill_rect_alpha(FBDev *fb,
                        int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
