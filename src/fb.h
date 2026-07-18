#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;
    uint8_t *mem;       /* mmap'd framebuffer, BGRX8888 */
    uint8_t *back;      /* off-screen back-buffer in RAM */
    int      width, height;
    int      stride;    /* bytes per row */
    int      n_pages;   /* 1 or 2 (double-buffered fbdev) */
    size_t   mmap_size; /* total mmap'd bytes (stride * height * n_pages) */
} FBDev;

int  fb_open(FBDev *fb, const char *path);
void fb_close(FBDev *fb);
void fb_clear(FBDev *fb);   /* clear back-buffer to black */
void fb_flip(FBDev *fb);    /* memcpy back->framebuffer */

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

/* Fill a rectangle in the back-buffer with a solid color + alpha blend. */
void fb_fill_rect_alpha(FBDev *fb,
                        int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
