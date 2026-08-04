/* All the decorative/audio-reactive drawing the app layers behind its
 * screens: the starfield (About/setup and a now-playing mode), rain,
 * Nebula (our audio-reactive plasma), the Toasty Squadron sprite port,
 * Now Spinning's CD disc + graphic EQ, the VU meters, and the PCM export
 * reader that feeds the reactive ones. Moved verbatim out of main.c —
 * see visualizers.h for the public surface; everything else in here is
 * internal state/tuning those effects own outright. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "visualizers.h"
#include "draw.h"
#include "util.h"
#include "stb_image.h"   /* declarations only — the implementation is compiled in main.c */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* Longest trail a star is allowed to draw, in pixel steps — a star that
 * respawns right in front of the camera can jump several screen-widths in
 * scale in one frame; without a cap that one frame draws a stray line
 * clean across the screen instead of a short streak. */
#define STAR_TRAIL_MAX 14

void draw_starfield(FBDev *fb)
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
        float z_prev = s->z;
        /* Was 0.012f, tuned back when this screen redrew at roughly half
         * its now-fixed rate (see g_fb_flip_count's comment) — halved to
         * land back at the approach speed it was tuned at. */
        s->z -= 0.006f;
        if (s->z <= 0.05f) { star_respawn(s); continue; }

        float scale = 1.0f / s->z;
        int sx = (int)(fb->width  / 2 + s->x * scale * (fb->width  / 4));
        int sy = (int)(fb->height / 2 + s->y * scale * (fb->height / 4));
        if (sx < 0 || sx >= fb->width || sy < 0 || sy >= fb->height) { star_respawn(s); continue; }

        int size = scale > 2.2f ? 2 : 1;   /* stars get a touch bigger as they approach */
        uint8_t bright = scale > 1.4f ? 255 : 150;

        /* Motion-blur trail: a short streak from where this star was last
         * frame to where it is now, fading in toward the head — same
         * "faster = more blur" self-scaling as the spinning disc's, just as a
         * fading streak instead of an averaged sample (better suited to a
         * point this small). Stars near the center barely move frame to
         * frame (short/no streak); ones rushing past the camera get a
         * proper hyperspace trail. */
        float scale_prev = 1.0f / z_prev;
        int psx = (int)(fb->width  / 2 + s->x * scale_prev * (fb->width  / 4));
        int psy = (int)(fb->height / 2 + s->y * scale_prev * (fb->height / 4));
        int dx = sx - psx, dy = sy - psy;
        int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (steps > STAR_TRAIL_MAX) steps = STAR_TRAIL_MAX;
        if (steps < 1) steps = 1;

        for (int t = 0; t <= steps; t++) {
            float frac = (float)t / steps;
            int px = psx + (int)(dx * frac + 0.5f);
            int py = psy + (int)(dy * frac + 0.5f);
            uint8_t alpha = (uint8_t)(60 + frac * frac * 195);   /* dim tail, full-bright head */
            fb_fill_rect_alpha(fb, px, py, size, size, bright, bright, bright, alpha);
        }
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
    /* Was 1.5f-4.0f, tuned back when this screen redrew at roughly half its
     * now-fixed rate (see g_fb_flip_count's comment) — halved to land back
     * at the fall speed it was tuned at. */
    d->speed = (1.5f + ((float)rand() / (float)RAND_MAX) * 2.5f) * 0.5f;
    d->bright = (uint8_t)(120 + rand() % 136);
}

void draw_rain(FBDev *fb)
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

/* Fireworks — the what's-new (changelog-before-install) screen's backdrop,
 * per user request: sporadically a single white pixel launches from the
 * bottom, climbs, and "pops" at its apex into a burst of colored particles
 * that scatter under gravity and fade out; a third of the particles carry
 * a short motion-blur streak (same trailing-arc idea as the starfield's).
 * Everything advances on real elapsed time (now_sec() deltas, same as
 * Toasty) so the pacing doesn't change with the redraw rate — the lesson
 * every per-frame-tuned effect above had to relearn when the redraw rate
 * doubled. Horizontal velocities are PAR-scaled so a burst reads as a
 * CIRCLE on the real 4:3 screen instead of a tall ellipse. */
#define FW_ROCKETS       6
#define FW_PARTICLES   360
#define FW_BURST_MIN    60
#define FW_BURST_VAR    40

typedef struct { float x, y, vx, vy; int alive; } FwRocket;
typedef struct {
    float   x, y, vx, vy;
    float   life, max_life;
    uint8_t r, g, b;
    int     blur, alive;
} FwParticle;

static FwRocket   fw_rockets[FW_ROCKETS];
static FwParticle fw_parts[FW_PARTICLES];
static double     fw_last_tick   = 0.0;
static double     fw_next_launch = 0.0;

/* One color family per burst — picked per rocket, with per-particle
 * brightness variation applied at spawn. */
static const uint8_t FW_COLORS[][3] = {
    { 0xFF, 0x50, 0x40 },   /* red        */
    { 0xFF, 0xC8, 0x40 },   /* gold       */
    { 0x50, 0xE0, 0xFF },   /* cyan       */
    { 0x60, 0xFF, 0x60 },   /* green      */
    { 0xFF, 0x60, 0xE0 },   /* magenta    */
    { 0xE8, 0xE8, 0xFF },   /* white-blue */
};
#define FW_COLOR_COUNT (int)(sizeof(FW_COLORS) / sizeof(FW_COLORS[0]))

static void fw_burst(FBDev *fb, float x, float y)
{
    const uint8_t *col = FW_COLORS[rand() % FW_COLOR_COUNT];
    int want = FW_BURST_MIN + rand() % FW_BURST_VAR;
    double par = par_correction(fb);
    for (int i = 0; i < FW_PARTICLES && want > 0; i++) {
        FwParticle *p = &fw_parts[i];
        if (p->alive) continue;
        want--;

        float ang   = ((float)rand() / (float)RAND_MAX) * 2.0f * (float)M_PI;
        float speed = fb->height * (0.10f + ((float)rand() / (float)RAND_MAX) * 0.28f);
        p->x  = x;
        p->y  = y;
        p->vx = cosf(ang) * speed * (float)par;
        p->vy = sinf(ang) * speed;
        p->max_life = 0.8f + ((float)rand() / (float)RAND_MAX) * 0.9f;
        p->life     = p->max_life;
        /* Same family, varied brightness, so the burst shimmers instead of
         * being one flat color. */
        float bright = 0.6f + ((float)rand() / (float)RAND_MAX) * 0.4f;
        p->r = (uint8_t)(col[0] * bright);
        p->g = (uint8_t)(col[1] * bright);
        p->b = (uint8_t)(col[2] * bright);
        p->blur  = (rand() % 3 == 0);
        p->alive = 1;
    }
}

void draw_fireworks(FBDev *fb)
{
    double now = now_sec();
    double dt = (fw_last_tick > 0.0) ? now - fw_last_tick : 0.0;
    fw_last_tick = now;
    if (dt < 0.0 || dt > 0.25) dt = 0.0;   /* clamp a long pause/clock jump */

    float grav = fb->height * 0.28f;   /* px/sec^2, gentle — embers drift down, not plummet */

    /* Sporadic launches: whenever the timer lapses AND a rocket slot is
     * free. The randomized gap is what makes it read as fireworks rather
     * than a fountain. */
    if (now >= fw_next_launch) {
        for (int i = 0; i < FW_ROCKETS; i++) {
            if (fw_rockets[i].alive) continue;
            FwRocket *rk = &fw_rockets[i];
            rk->x  = fb->width * (0.12f + ((float)rand() / (float)RAND_MAX) * 0.76f);
            rk->y  = (float)fb->height + 2.0f;
            rk->vx = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * fb->width * 0.02f;
            /* Wide launch-speed spread so apex height (and how long the
             * climb takes) varies a lot burst to burst — a weak launch
             * pops low and quick, a strong one climbs slowly and pops
             * near the top; previously this was too narrow a range and
             * every rocket ended up popping at the very top. */
            rk->vy = -fb->height * (0.32f + ((float)rand() / (float)RAND_MAX) * 0.50f);
            rk->alive = 1;
            break;
        }
        fw_next_launch = now + 0.35 + ((double)rand() / RAND_MAX) * 1.0;
    }

    for (int i = 0; i < FW_ROCKETS; i++) {
        FwRocket *rk = &fw_rockets[i];
        if (!rk->alive) continue;
        rk->x  += rk->vx * (float)dt;
        rk->y  += rk->vy * (float)dt;
        rk->vy += grav * 0.9f * (float)dt;   /* decelerating climb */

        /* Pop at the apex (climb spent), or at a ceiling if it was fast
         * enough to threaten the very top of the screen. */
        if (rk->vy > -fb->height * 0.06f || rk->y < fb->height * 0.12f) {
            rk->alive = 0;
            fw_burst(fb, rk->x, rk->y);
            continue;
        }

        int sx = (int)rk->x, sy = (int)rk->y;
        /* White head + a faint 2-step tail BELOW it (it's climbing). */
        fb_fill_rect_alpha(fb, sx, sy,     1, 1, 255, 255, 255, 255);
        fb_fill_rect_alpha(fb, sx, sy + 2, 1, 1, 255, 255, 255, 120);
        fb_fill_rect_alpha(fb, sx, sy + 4, 1, 1, 255, 255, 255, 50);
    }

    for (int i = 0; i < FW_PARTICLES; i++) {
        FwParticle *p = &fw_parts[i];
        if (!p->alive) continue;

        float px_prev = p->x, py_prev = p->y;
        p->x  += p->vx * (float)dt;
        p->y  += p->vy * (float)dt;
        p->vy += grav * (float)dt;
        p->life -= (float)dt;
        if (p->life <= 0.0f || p->y >= fb->height || p->x < -4.0f || p->x >= fb->width + 4.0f) {
            p->alive = 0;
            continue;
        }

        uint8_t alpha = (uint8_t)(255.0f * (p->life / p->max_life));
        int sx = (int)p->x, sy = (int)p->y;
        if ((unsigned)sx < (unsigned)fb->width && (unsigned)sy < (unsigned)fb->height)
            fb_fill_rect_alpha(fb, sx, sy, 1, 1, p->r, p->g, p->b, alpha);

        if (p->blur) {
            /* Short streak back toward where it was last frame — dimmer
             * than the head, same self-scaling-with-speed idea as the
             * starfield trails. */
            int bx = (int)((px_prev + p->x) * 0.5f), by = (int)((py_prev + p->y) * 0.5f);
            int tx = (int)px_prev, ty = (int)py_prev;
            if ((unsigned)bx < (unsigned)fb->width && (unsigned)by < (unsigned)fb->height)
                fb_fill_rect_alpha(fb, bx, by, 1, 1, p->r, p->g, p->b, (uint8_t)(alpha / 2));
            if ((unsigned)tx < (unsigned)fb->width && (unsigned)ty < (unsigned)fb->height)
                fb_fill_rect_alpha(fb, tx, ty, 1, 1, p->r, p->g, p->b, (uint8_t)(alpha / 4));
        }
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

void draw_nebula(FBDev *fb, const int16_t *samples, int n)
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
    /* Every per-call increment here was tuned while this screen redrew at
     * roughly half its now-fixed rate (see g_fb_flip_count's comment —
     * STATE_PLAYING_AUDIO had the same redundant-usleep bug STATE_BROWSE
     * did, just not yet found/fixed when Nebula was last tuned), so at the
     * same numbers everything advanced about twice as fast in real time.
     * ALL of them are compensated to land back at the tuned look: the
     * swirl/attractor/zoom/palette increments are halved, and decay —
     * multiplicative per frame, not additive — moves toward 1 so that
     * applying it twice as often fades a trail at the same per-second
     * rate as before (0.949^2sec-worth ~= 0.973 per new frame -> ~249/256;
     * a first pass halved only swirl/attractor and the user immediately
     * spotted the rest still running fast). */
    nebula_swirl += (0.010 + energy * 0.05) * 0.5;
    double theta = 0.022 * sin(nebula_swirl);
    double zoom  = 1.0 - (0.010 + energy * 0.018) * 0.5;   /* <1 => content flows */
    double ct = cos(theta) / zoom, st = sin(theta) / zoom;
    double cx = NEBULA_W / 2.0, cy = NEBULA_H / 2.0;
    unsigned decay = 249;   /* /256 per frame */

    /* On top of the global spin, one gentle "circular attractor" drifting
     * behind the cover adds a small bounded radial pull + tangential swirl to
     * where the feedback is sampled — just enough to curl the waveform,
     * deliberately weak so it doesn't suck the whole image into a whirlpool.
     * Louder audio strengthens it a little. */
    nebula_t += (0.006 + energy * 0.02) * 0.5;   /* see the swirl increment's own comment */
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
    nebula_hue += (target_hue - nebula_hue) * 0.025;               /* see the swirl block's rate-compensation comment */
    nebula_pal_phase += (0.0015 + energy * 0.010) * 0.5;   /* only a slow in-scheme drift */
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

void toasty_load(FBDev *fb)
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

/* The SELECT handler in main() draws one normal frame BEFORE triggering
 * the multi-second first load (see draw_toasty_bg's own comment) — this
 * is what it checks to know whether that's still needed. */
int toasty_is_loaded(void)
{
    return g_toasty_loaded;
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
void draw_toasty_bg(FBDev *fb)
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
void draw_toasty_fg(FBDev *fb)
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
void draw_now_playing_gradient(FBDev *fb)
{
    for (int y = 0; y < fb->height; y++) {
        uint8_t a = (uint8_t)(255 * y / (fb->height - 1));
        fb_fill_rect_alpha(fb, 0, y, fb->width, 1, 0, 0, 0, a);
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
int read_af_samples(int16_t *buf, int max_samples)
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
void draw_vu_horizontal(FBDev *fb, const int16_t *samples, int count,
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

/* Now Spinning's cover art: the immersive cover rendered as a spinning CD
 * (circular, with a hole punched in the center) instead of a plain
 * rectangle. max_h is the same "physical box height" the other immersive
 * modes pass to blit_fit_centered — the disc's on-screen diameter matches
 * their cover size exactly.
 *
 * Nearest-neighbor per destination pixel, same technique as fb_blit: for
 * each screen pixel inside the disc, rotate its position back by -angle
 * and sample the cover at that point, so the RESULT spins while the
 * sampling math itself stays a simple lookup (no separate rotate-then-
 * blit pass, no intermediate buffer). Source is treated as its own
 * center-cropped square (album art is square in practice; a non-square
 * source just loses its longer edge rather than distorting).
 *
 * blur_span is how far (radians) the disc turned since the last redraw —
 * at DISC_SPEED's higher settings that's tens of degrees per frame, which
 * without any blur reads as strobing rather than spinning (no motion blur
 * to fake what a real fast-spinning disc's own persistence of vision
 * would show). Instead of one sample per pixel, CD_BLUR_TAPS samples are
 * taken across the swept arc and averaged — a cheap "shutter" rather than
 * a persistent accumulation buffer, so it costs nothing extra when the
 * disc is slow or paused (span near 0 collapses back to one sample) and
 * scales itself to however fast the disc is actually turning. */
#define CD_HOLE_FRAC 0.16   /* hole radius, as a fraction of the disc radius */
#define CD_BLUR_TAPS 5

void draw_now_playing_cd(FBDev *fb, const uint8_t *px, int sw, int sh,
                                 int cx, int cy, int max_h, double angle, double blur_span)
{
    if (!px || sw <= 0 || sh <= 0 || max_h <= 0) return;

    double par = par_correction(fb);
    int ry = max_h / 2;
    /* rx = ry*par is what makes the disc a true circle ON SCREEN rather
     * than an oval — same physical-square relationship blit_fit_centered's
     * own dw/dh math relies on (see its comment). */
    int rx = (int)(ry * par + 0.5);
    if (rx < 1 || ry < 1) return;

    int x0 = cx - rx, x1 = cx + rx, y0 = cy - ry, y1 = cy + ry;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->width)  x1 = fb->width;
    if (y1 > fb->height) y1 = fb->height;

    int side = sw < sh ? sw : sh;   /* centered square crop of the source */
    int off_x = (sw - side) / 2, off_y = (sh - side) / 2;
    double hole2 = CD_HOLE_FRAC * CD_HOLE_FRAC;

    /* Taps span [angle - blur_span, angle] — the disc's own trailing arc,
     * same direction a real motion blur streaks in. */
    double cs[CD_BLUR_TAPS], sn[CD_BLUR_TAPS];
    for (int k = 0; k < CD_BLUR_TAPS; k++) {
        double a = angle - blur_span * ((double)k / (CD_BLUR_TAPS - 1));
        cs[k] = cos(a); sn[k] = sin(a);
    }

    for (int fy = y0; fy < y1; fy++) {
        double v = (double)(fy - cy) / ry;
        uint32_t *dst_row = (uint32_t *)(fb->back + (size_t)fy * fb->stride);
        for (int fx = x0; fx < x1; fx++) {
            double u = (double)(fx - cx) / rx;
            double r2 = u * u + v * v;
            if (r2 > 1.0 || r2 < hole2) continue;   /* outside the disc, or in the hole */

            uint32_t acc_r = 0, acc_g = 0, acc_b = 0;
            for (int k = 0; k < CD_BLUR_TAPS; k++) {
                /* Rotate the sample point by -angle so the IMAGE appears to
                 * spin by +angle. */
                double ru = u * cs[k] + v * sn[k];
                double rv = -u * sn[k] + v * cs[k];

                int sx = (int)((ru * 0.5 + 0.5) * side);
                int sy = (int)((rv * 0.5 + 0.5) * side);
                if (sx < 0) sx = 0; else if (sx >= side) sx = side - 1;
                if (sy < 0) sy = 0; else if (sy >= side) sy = side - 1;

                const uint8_t *src = px + (size_t)(sy + off_y) * sw * 4 + (size_t)(sx + off_x) * 4;
                acc_r += src[0]; acc_g += src[1]; acc_b += src[2];
            }
            dst_row[fx] = ((acc_r / CD_BLUR_TAPS) << 16) | ((acc_g / CD_BLUR_TAPS) << 8) | (acc_b / CD_BLUR_TAPS);
        }
    }
}

/* Now Spinning's own background: a classic multi-band graphic-EQ bar, full width,
 * blocky LED-style segments colored green/yellow/red by height (bottom to
 * top), per user reference. No real FFT (this codebase has never done one
 * — Nebula above reads raw PCM the same way): each band is a cheap RBJ
 * resonant bandpass biquad, log-spaced across a nominal 100Hz-8kHz voice/
 * music range, run over the same live PCM chunk read_af_samples already
 * hands the VU meters. A real spectrum would need a known sample rate and
 * an FFT; this doesn't need to be correct, just visibly frequency-
 * selective and reactive — the assumed 44.1kHz is close enough for a
 * decorative visualizer regardless of the stream's actual rate. */
#define EQ_BANDS   24
#define EQ_SEGS     8
#define EQ_GAIN   3.0   /* bandpass output is much quieter than raw amplitude — tune to taste.
                          * Was 7.0: fine against the isolated test tones this was built with,
                          * but real music has broadband content in every band at once, so most
                          * bars sat pinned at the top (stuck on red) instead of visibly moving
                          * with the track — lowered so typical program material leaves headroom
                          * for the smoothing below to actually show movement. */

static double eq_b0[EQ_BANDS], eq_b2[EQ_BANDS], eq_a1[EQ_BANDS], eq_a2[EQ_BANDS];
static double eq_x1[EQ_BANDS], eq_x2[EQ_BANDS], eq_y1[EQ_BANDS], eq_y2[EQ_BANDS];
static double eq_level[EQ_BANDS];
static int    eq_coeffs_ready = 0;

static void eq_init_coeffs(void)
{
    if (eq_coeffs_ready) return;
    eq_coeffs_ready = 1;
    const double fs = 44100.0, f_lo = 100.0, f_hi = 8000.0, q = 2.5;
    for (int i = 0; i < EQ_BANDS; i++) {
        double f0 = f_lo * pow(f_hi / f_lo, (double)i / (EQ_BANDS - 1));   /* log-spaced centers */
        double w0 = 2.0 * M_PI * f0 / fs;
        double alpha = sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        eq_b0[i] =  alpha / a0;          /* RBJ constant-skirt bandpass, b1 == 0 */
        eq_b2[i] = -alpha / a0;
        eq_a1[i] = -2.0 * cos(w0) / a0;
        eq_a2[i] =  (1.0 - alpha) / a0;
    }
}

void draw_now_playing_eq(FBDev *fb, const int16_t *samples, int n,
                                 int x0, int y0, int w, int h)
{
    eq_init_coeffs();
    int half = n / 2;   /* left channel only — same convention as draw_nebula/the VU meters */

    int gap = 2;
    int bar_w = (w - (EQ_BANDS - 1) * gap) / EQ_BANDS;
    if (bar_w < 1) bar_w = 1;
    int seg_gap = 1;
    int seg_h = (h - (EQ_SEGS - 1) * seg_gap) / EQ_SEGS;
    if (seg_h < 1) seg_h = 1;

    for (int b = 0; b < EQ_BANDS; b++) {
        double x1 = eq_x1[b], x2 = eq_x2[b], y1 = eq_y1[b], y2 = eq_y2[b];
        double b0 = eq_b0[b], b2 = eq_b2[b], a1 = eq_a1[b], a2 = eq_a2[b];
        double peak = 0.0;
        for (int i = 0; i < half; i++) {
            double xn = samples[i] / 32768.0;
            double yn = b0 * xn + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = xn; y2 = y1; y1 = yn;
            double a = yn < 0 ? -yn : yn;
            if (a > peak) peak = a;
        }
        eq_x1[b] = x1; eq_x2[b] = x2; eq_y1[b] = y1; eq_y2[b] = y2;

        /* count==0 (paused) leaves peak at 0, so bars decay to empty via
         * the smoothing below instead of freezing — same reasoning as
         * draw_vu_horizontal. */
        double raw = peak * EQ_GAIN;
        if (raw > 1.0) raw = 1.0;
        double lvl = eq_level[b];
        const double attack = 0.6, decay = 0.16;   /* decay was 0.10 — quicker fall so a bar
                                                      * that peaked doesn't linger up there
                                                      * once the music's actual level has
                                                      * already dropped. */
        lvl += (raw - lvl) * (raw > lvl ? attack : decay);
        eq_level[b] = lvl;

        int lit = (int)(lvl * EQ_SEGS + 0.5);
        int bx = x0 + b * (bar_w + gap);
        for (int s = 0; s < EQ_SEGS; s++) {
            uint8_t r, g, bl;
            if (s < EQ_SEGS * 6 / 10)       { r = 0x30; g = 0xE0; bl = 0x30; }
            else if (s < EQ_SEGS * 85 / 100) { r = 0xE0; g = 0xC0; bl = 0x20; }
            else                              { r = 0xE0; g = 0x30; bl = 0x30; }
            /* Dim, not invisible, when unlit — reads as an LED strip with
             * its off segments still faintly visible, like the reference. */
            uint8_t alpha = (s < lit) ? 255 : 50;
            int sy = y0 + h - (s + 1) * (seg_h + seg_gap);
            fb_fill_rect_alpha(fb, bx, sy, bar_w, seg_h, r, g, bl, alpha);
        }
    }
}

/* Now Spinning disc rotation angle, eased toward 0 speed when paused and back up to
 * full speed on resume — a hard stop/start read as a glitch (the disc is
 * meant to look like a physical thing spinning under a laser, not a sprite
 * turning on a fixed clock). State persists across calls: disc_speed is the
 * disc's current angular velocity, eased toward whichever target (0 when
 * paused, DISC_SPEED otherwise) with a time-constant so it's frame-rate
 * independent — same reasoning as draw_vu_horizontal's own smoothing, just
 * exponential instead of a fixed per-tick step. */
double now_spinning_disc_angle(int paused, double *out_blur_span)
{
    #define DISC_SPEED 11.25    /* rad/sec — a real CD spins at ~21-52 rad/s (200-500 RPM),
                                  * which would just be a blur at this resolution/frame rate.
                                  * Halved from 22.5 per user feedback (motion blur scales with
                                  * speed, so this also halves the blur strength). */
    #define DISC_SPINDOWN_TAU 0.7 /* seconds to mostly settle into the new speed */
    static double angle = 0.0, speed = 0.0, last_t = -1.0;

    double t = now_sec();
    double dt = (last_t < 0.0) ? 0.0 : t - last_t;
    if (dt > 0.5) dt = 0.5;   /* guard a long gap (screen not visited in a while) from one huge jump */
    last_t = t;

    double target = paused ? 0.0 : DISC_SPEED;
    double ease = 1.0 - exp(-dt / DISC_SPINDOWN_TAU);
    speed += (target - speed) * ease;
    angle += speed * dt;

    /* How far the disc swept THIS frame — draw_now_playing_cd's motion-
     * blur tap span (see its own comment). Naturally 0 when paused/just
     * resumed (speed near 0) and grows with DISC_SPEED, no separate
     * tuning knob needed. */
    *out_blur_span = speed * dt;

    double a = fmod(angle, 2.0 * M_PI);
    if (a < 0.0) a += 2.0 * M_PI;
    return a;
    #undef DISC_SPEED
    #undef DISC_SPINDOWN_TAU
}
