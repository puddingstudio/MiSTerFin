#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

/* Monotonic seconds — the one timebase everything animates and times
 * against (immune to wall-clock jumps, unlike time()). */
double now_sec(void);

/* stb_image decode of a downloaded scratch file to RGBA8888 (caller frees),
 * deleting the file afterwards — the standard step between "curl wrote the
 * image somewhere" and "pixels ready to blit". */
uint8_t *load_image_tmp(const char *tmp_path, int *w, int *h);

/* Like load_image_tmp but KEEPS the file — for reading a persistent cache
 * file off the SD card (the cover cache), where deleting it would defeat the
 * whole point. */
uint8_t *load_image_keep(const char *path, int *w, int *h);

#endif
