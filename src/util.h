#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic seconds — the one timebase everything animates and times
 * against (immune to wall-clock jumps, unlike time()). */
double now_sec(void);

/* stb_image decode of a downloaded scratch file to RGBA8888 (caller frees),
 * deleting the file afterwards — the standard step between "curl wrote the
 * image somewhere" and "pixels ready to blit". */
uint8_t *load_image_tmp(const char *tmp_path, int *w, int *h);

/* Like load_image_tmp but KEEPS the file — for reading a persistent cache
 * file, where deleting it would defeat the whole point. */
uint8_t *load_image_keep(const char *path, int *w, int *h);

/* Builds one cache-directory path beneath MISTERFIN_CACHE_ROOT. The MiSTer
 * install directory remains the default; the desktop harness overrides the
 * root so the same artwork cache code works without a /media/fat mount. */
int cache_dir_path(const char *name, char *out, size_t outsz);

/* Creates the configured cache root and named child directory if needed. */
int cache_dir_ensure(const char *name, char *out, size_t outsz);

/* Deletes cache entries left behind by a superseded filename key: every
 * regular file directly in `dir` whose name ends in `ext` and does NOT
 * contain `keep_substr`. Returns how many were removed.
 *
 * For the on-disk cover and grid caches, whose names carry the width the
 * artwork was fetched at ("_w180"). Changing a fetch width makes the old
 * files unreachable by key and strands them on the card forever, so this
 * reclaims them.
 *
 * Effectively runs once without needing a marker file: every current name
 * contains `keep_substr` by construction, so after the first pass nothing
 * matches — and the live cache can never be a candidate, on this run or any
 * later one. Preferred over a marker because it still works for a card
 * restored from an old backup, which a marker would skip.
 *
 * Deliberately narrow: only that one directory, only regular files, only that
 * extension — an unrelated file someone dropped in there survives. And safe
 * to be wrong about in either direction, because these are caches the app
 * already refills in the background whenever an entry is missing. */
int cache_sweep_superseded(const char *dir, const char *ext, const char *keep_substr);

#endif
