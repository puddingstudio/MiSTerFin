#ifndef VISUALIZERS_H
#define VISUALIZERS_H

#include <stdint.h>
#include "fb.h"

/* The decorative/audio-reactive background effects and their supporting
 * pieces (PCM reader, VU meters) — everything here draws into fb->back
 * and owns its own animation state internally. Which effect a screen uses
 * and when stays main.c's decision (g_now_playing_bg and friends); this
 * layer only knows how to draw them. */

/* mplayer's -af export target. Shared on purpose: play_audio() (main.c)
 * builds the mplayer command line around it, read_af_samples() below
 * reads it back, and play_audio also unlinks it between tracks so a new
 * track can't briefly visualize the previous one's stale tail. */
#define AF_EXPORT_PATH "/tmp/misterfin_af_export"

/* Reads mplayer's live PCM export fresh every call — returns sample count
 * (left channel then right channel, int16), 0 if the file isn't there
 * yet. See the implementation's comment for why the header is ignored. */
int read_af_samples(int16_t *buf, int max_samples);

/* "Flying through stars", with per-star motion-blur trails — About screen,
 * the setup/Quick Connect error screens, and now-playing mode 0. */
void draw_starfield(FBDev *fb);

/* Falling blue-white rain streaks — now-playing mode 1. */
void draw_rain(FBDev *fb);

/* Fireworks: sporadic white rockets from the bottom edge popping into
 * colored, gravity-scattered bursts — the what's-new screen's backdrop. */
void draw_fireworks(FBDev *fb);

/* Nebula, the audio-reactive plasma (now-playing mode 2). samples/n come
 * from read_af_samples; n == 0 (paused / not exporting yet) is fine. */
void draw_nebula(FBDev *fb, const int16_t *samples, int n);

/* Toasty Squadron (now-playing mode 4). The frame decode takes multiple
 * seconds on this hardware, so it's triggered EXPLICITLY (toasty_load,
 * spinner shown on fb) by the SELECT handler that switches to the mode —
 * after it has drawn one normal frame — rather than lazily from the draw
 * call; draw_toasty_bg silently no-ops until loaded. The bg pass advances
 * all sprites and draws every tier but mega; draw_toasty_fg draws the
 * mega tier OVER the screen's own UI, right before fb_flip. */
void toasty_load(FBDev *fb);
int  toasty_is_loaded(void);
void draw_toasty_bg(FBDev *fb);
void draw_toasty_fg(FBDev *fb);

/* Top-transparent to bottom-black dim between a busy effect and the
 * now-playing screen's own UI (used by the Toasty mode). */
void draw_now_playing_gradient(FBDev *fb);

/* Classic recording-console horizontal level meter. *level is persistent
 * attack/decay smoothing state, owned by the caller, one double per
 * meter. count == 0 lets the meter decay to empty. */
void draw_vu_horizontal(FBDev *fb, const int16_t *samples, int count,
                        int x0, int y, int w, int height, double *level);

/* Now Spinning (now-playing mode 3): the cover as a spinning CD (angle/
 * blur_span from now_spinning_disc_angle) over a full-width graphic-EQ
 * bar fed the same samples as everything else. */
void draw_now_playing_cd(FBDev *fb, const uint8_t *px, int sw, int sh,
                         int cx, int cy, int max_h, double angle, double blur_span);
void draw_now_playing_eq(FBDev *fb, const int16_t *samples, int n,
                         int x0, int y0, int w, int h);
double now_spinning_disc_angle(int paused, double *out_blur_span);

#endif
