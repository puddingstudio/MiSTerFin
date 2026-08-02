#ifndef GRID_H
#define GRID_H

#include "fb.h"
#include "jellyfin.h"

/* The home carousel's dimmed cover-art mosaic background: per-library cover
 * caches (RAM for the session, SD card across launches), a silent background
 * prefetch of every library, and the slow direction-alternating crawl it's
 * drawn with. Owns all of its own state — the only thing it needs from the
 * app is the server config, handed over once via grid_init(). */

/* Stores the config pointer every later download/count call uses. Call once
 * at startup, before anything else here — the pointed-to config must outlive
 * the app (it's main.c's global). */
void grid_init(const JfConfig *cfg);

/* Makes `view`'s mosaic the active one, populating its cache slot first if
 * this is the first visit (blocking: disk cache, else network fetch with a
 * corner spinner on `fb`). Main thread only. */
void grid_covers_sync(FBDev *fb, const JfItem *view);

/* Kicks off the background cover-grid prefetch. Deliberately not started
 * unconditionally at launch: it needs a resolved user and a working
 * credential, and starting it from the setup or Quick Connect screens meant
 * firing a /UserViews with an empty userId and no token — harmless against a
 * permissive server, a guaranteed 401 against a real one, and a confusing
 * entry in the log for anyone debugging why their client won't connect.
 * Called from both paths that reach a signed-in library. `fb` must be the
 * long-lived FBDev from main() — the thread reads its geometry. */
void start_grid_prefetch(FBDev *fb);

/* Draws the active library's scrolling mosaic onto the (just-cleared, black)
 * back buffer. No-op until a library's cache is ready. */
void draw_grid_background(FBDev *fb);

/* Vertical black gradient over the cover grid, under everything else (top
 * bar, cards, hint) — transparent at the top, solid at the bottom, per
 * user request, so the grid stays visible near the top but doesn't fight
 * with the hint text/cards lower down. */
void draw_grid_gradient(FBDev *fb);

#endif
