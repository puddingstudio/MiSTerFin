#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include "fb.h"

/* Writes the current on-screen framebuffer to a timestamped BMP under
 * /media/fat/screenshots/MiSTerFin/, and flashes a brief on-screen
 * confirmation. Call on the SELECT+START combo (see
 * input_select_start_held() in input.h) from any app state — it reads
 * whatever's actually on screen via fb->mem, so it works during video
 * playback too. */
void screenshot_take(FBDev *fb);

#endif
