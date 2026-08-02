#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>
#include "fb.h"

/* Drawing primitives: the 8x8 bitmap-font text renderer and the aspect-
 * corrected image blit. Screen composition (browse lists, carousel, info
 * screen, ...) stays in main.c — this is only the layer below it, the
 * parts that know nothing about the app. */

/* ── colour palette ─────────────────────────────────────────────────────
 * Each expands to r,g,b so it drops straight into draw_text's trailing
 * uint8_t triple. */
#define COL_TITLE   0xFF,0xE0,0x40
#define COL_ITEM    0xCC,0xCC,0xCC
#define COL_DIM     0x80,0x80,0x80
#define COL_SEL_BG  0x10,0x40,0x90
#define COL_SEL_FG  0xFF,0xFF,0xFF
#define COL_HINT    0x80,0x80,0x80
#define COL_WATCHED 0x40,0xCC,0x40
#define COL_RESUME  0xFF,0xC0,0x40
#define COL_ERR     0xFF,0x60,0x60

/* Text is UTF-8; one glyph cell per code point (Latin-1 covered, anything
 * else draws '?'). scale multiplies the 8x8 cell. */
void draw_text(FBDev *fb, int x, int y, const char *s, int scale,
               uint8_t r, uint8_t g, uint8_t b);
int  text_width(FBDev *fb, const char *s, int scale);

/* Like draw_text, but skips any glyph cell that doesn't fit entirely
 * inside [clip_x0, clip_x1) — for marquee scrolling. */
void draw_text_clipped(FBDev *fb, int x, int y, const char *s, int scale,
                       uint8_t r, uint8_t g, uint8_t b, int clip_x0, int clip_x1);

/* Truncates s in place (with a ".." tail) to fit max_w pixels. */
void truncate_to_width(FBDev *fb, char *s, int scale, int max_w);

/* Word-wraps text into at most max_lines lines of max_w pixels; returns
 * the y just below the last line drawn. */
int  draw_wrapped(FBDev *fb, int x, int y, const char *text,
                  int scale, int max_w, int max_lines,
                  uint8_t r, uint8_t g, uint8_t b);

/* Pixel-aspect-ratio correction: this framebuffer's pixels aren't square
 * on a 4:3 display, so image widths need stretching by this factor to
 * look right (5/3 at PAL's 288 active lines). */
double par_correction(FBDev *fb);

/* Corner loading indicator. SPINNER_SIZE/SPINNER_MARGIN are public because
 * the top bar's clock positions itself clear of the spinner's square. */
#define SPINNER_SIZE   14
#define SPINNER_MARGIN 10

/* Simple blinking square in the top-right corner — a GIF mascot animation
 * was tried first but wasn't worth the decode/ghosting complexity for what
 * is just a "something's loading" cue. */
void draw_spinner_frame(FBDev *fb, int frame_idx);

/* Aspect-preserving blit of src into a max_w x max_h box centered on
 * (cx, cy), applying par_correction. */
void blit_fit_centered(FBDev *fb, const uint8_t *src, int sw, int sh,
                       int cx, int cy, int max_w, int max_h, uint8_t alpha);

#endif
