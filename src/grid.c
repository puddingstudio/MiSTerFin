#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include "grid.h"
#include "draw.h"
#include "util.h"

/* Separate download scratch path for the background grid-cache prefetch
 * thread (see grid_prefetch_thread()) — it must never share the main
 * thread's scratch path with an in-flight download (info screen backdrop/
 * logo, browse cover panel, etc.), which would silently clobber whichever
 * one finishes last. GRID_POSTER_TMP is that main-thread scratch path —
 * the SAME file main.c's POSTER_TMP names for its own main-thread
 * downloads (grid_covers_sync runs on the main thread too, so sharing it
 * is deliberate and safe; keep the two in sync). */
#define GRID_POSTER_TMP    "/tmp/misterfin_poster.img"
#define GRID_POSTER_TMP_BG "/tmp/misterfin_poster_bg.img"
#define GRID_CACHE_DIR     "/media/fat/misterfin/gridcache"

/* Dimmed cover-art grid behind the carousel, tiling whatever the active
 * library actually contains — one library's worth of covers cached at a
 * time — but keeps EVERY library's grid it has ever loaded cached in its
 * own slot for the rest of the app session (not just the current one):
 * flipping back to a previously-visited library was re-downloading and
 * re-decoding its covers from scratch every single time otherwise (visible
 * as a lag spike per switch), confirmed as the cause by the user. Bounded
 * to GRID_LIB_CACHE_MAX slots — comfortably above any realistic number of
 * Jellyfin libraries, and each slot is only a handful of small (100px-wide)
 * decoded images, so the memory cost of never evicting is trivial on this
 * platform's RAM. Uses its own JfItem buffer (grid_items in
 * grid_cache_populate), NOT the browse list's items — that array is the
 * carousel's own selection list and must not be clobbered by this side
 * listing. */
#define GRID_FETCH_MAX 12
#define GRID_COLS 6          /* covers per row — was 8, made bigger per user request */
#define GRID_ROWS_MAX 8      /* array sizing only — actual row count is per-cache, see .rows */
#define GRID_ALPHA 110   /* out of 255 — dimmed but covers should read clearly, per user feedback that 40 then 65 were still too faint */
#define GRID_LIB_CACHE_MAX 16
/* Slow, continuous horizontal crawl of the grid background, alternating
 * direction row-to-row (row 0 right-to-left, row 1 left-to-right, ...) —
 * per user request. Kept gentle on purpose ("lagano ne brzo"): at this
 * speed a GRID_COLS=6 cell (~106px wide) takes about 10 seconds to fully
 * cycle. */
#define GRID_SCROLL_PX_PER_SEC 10.0

typedef struct {
    char     view_id[JF_ID_LEN];
    uint8_t *px[GRID_FETCH_MAX];
    int      w[GRID_FETCH_MAX], h[GRID_FETCH_MAX];
    int      count;
    int      square;   /* 1 for a music library, 0 otherwise — see draw_grid_background */
    int      rows;      /* how many rows to draw — generous on purpose, see grid_cache_populate */
    /* Which cover (index into px[]) each grid cell shows — shuffled once
     * when this slot is first filled, NOT per draw: draw_browse_carousel
     * redraws every ~100ms just for the clock/marquee tick even with no
     * navigation, so reshuffling per-draw would make the background
     * visibly jitter. Sized for the largest possible row count (.rows may
     * be smaller); only the first GRID_COLS*rows entries are ever read. */
    int      cell_order[GRID_COLS * GRID_ROWS_MAX];
    /* Set only once grid_cache_populate() fully finishes — a slot's
     * view_id becomes visible to other threads' scans (under g_grid_mutex)
     * as soon as it's reserved, before population (which can take a while
     * and deliberately runs without the lock held) completes. Checked by
     * draw_grid_background so it never renders a slot mid-fill. */
    int      ready;
} GridLibCache;

static GridLibCache g_grid_cache[GRID_LIB_CACHE_MAX];
static int           g_grid_cache_n  = 0;    /* slots filled so far */
static int           g_grid_active   = -1;   /* index of the currently-shown library's slot, -1 = none */
/* Protects g_grid_cache/g_grid_cache_n/g_grid_active — grid_covers_sync()
 * (main thread, interactive) and grid_prefetch_thread() (background,
 * launched once from the home screen) both touch these. */
static pthread_mutex_t g_grid_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Server config, handed over once by grid_init() — points at main.c's
 * global, which outlives everything here. */
static const JfConfig *s_cfg;

void grid_init(const JfConfig *cfg)
{
    s_cfg = cfg;
}

/* A plain Fisher-Yates shuffle of the (count-cycled) index list still reads
 * as "repeating" with few unique covers spread over many cells (e.g. 4
 * covers over this grid's 32 cells is 8 copies of each) — nothing stops the
 * same cover landing in two vertically- or horizontally-adjacent cells,
 * which is exactly the visible pattern a uniform shuffle doesn't avoid.
 * Build the order cell-by-cell instead, rejecting a pick that matches the
 * cell directly above or to the left (a few retries, not exhaustive) —
 * cheap and enough to kill the obvious adjacent-repeat look; only gives up
 * (accepting a repeat) when there aren't enough distinct covers to satisfy
 * both neighbors at once. */
static void grid_cell_order_shuffle(GridLibCache *gc)
{
    if (gc->count <= 0) {
        for (int i = 0; i < GRID_COLS * gc->rows; i++) gc->cell_order[i] = 0;
        return;
    }
    for (int row = 0; row < gc->rows; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int left  = col > 0 ? gc->cell_order[row * GRID_COLS + col - 1] : -1;
            int above = row > 0 ? gc->cell_order[(row - 1) * GRID_COLS + col] : -1;
            int pick, tries = 0;
            do {
                pick = rand() % gc->count;
            } while (gc->count > 2 && (pick == left || pick == above) && ++tries < 8);
            gc->cell_order[row * GRID_COLS + col] = pick;
        }
    }
}

/* Turns a Jellyfin GUID view_id into a safe filename — GUIDs are already
 * hex+dashes so this is just a defensive fallback, not real sanitizing. */
static void grid_cache_disk_path(const char *view_id, char *out, size_t outsz)
{
    char safe[JF_ID_LEN];
    size_t j = 0;
    for (size_t i = 0; view_id[i] && j < sizeof(safe) - 1; i++) {
        char c = view_id[i];
        safe[j++] = (isalnum((unsigned char)c) || c == '-') ? c : '_';
    }
    safe[j] = '\0';
    snprintf(out, outsz, GRID_CACHE_DIR "/%s.dat", safe);
}

/* Persisted grid cache survives an app restart — without it, every
 * library's mosaic had to be re-downloaded/re-decoded from scratch on
 * every single launch even though nothing in the library had changed,
 * confirmed as a real lag spike by the user. Freshness check is a single
 * cheap jf_count_items() (Limit=0) call: if the library's total item count
 * still matches what it was when this was written, trust the cache as-is.
 * Doesn't catch a same-count swap (one item replaced by another), but
 * that's a rare edge case for what's purely decorative background art.
 * Pixels are stored raw/uncompressed rather than re-encoded to JPEG (this
 * build's stb_image.h is decode-only, no encoder) — at most ~480KB per
 * library (12 covers * ~100x100x4 bytes), trivial for an SD card. */
static int grid_cache_load_from_disk(GridLibCache *gc, const JfItem *view, const char *item_type)
{
    int64_t current_count = jf_count_items(s_cfg, view->id, item_type);
    if (current_count < 0) return 0;

    char path[300];
    grid_cache_disk_path(view->id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int64_t cached_count = -1;
    int32_t n_covers = 0;
    if (fread(&cached_count, sizeof(cached_count), 1, f) != 1 ||
        fread(&n_covers, sizeof(n_covers), 1, f) != 1 ||
        cached_count != current_count || n_covers <= 0 || n_covers > GRID_FETCH_MAX) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < n_covers; i++) {
        int32_t w = 0, h = 0;
        size_t need;
        if (fread(&w, sizeof(w), 1, f) != 1 || fread(&h, sizeof(h), 1, f) != 1 ||
            /* Bounded individually before multiplying: size_t is 32-bit on
             * the ARM target, so w*h*4 can wrap and slip under the 4MB guard
             * on a corrupted cache file. */
            w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
            (need = (size_t)w * (size_t)h * 4) > 4 * 1024 * 1024) {
            fclose(f);
            for (int k = 0; k < gc->count; k++) { free(gc->px[k]); gc->px[k] = NULL; }
            gc->count = 0;
            return 0;
        }
        uint8_t *px = malloc(need);
        if (!px || fread(px, 1, need, f) != need) {
            free(px);
            fclose(f);
            for (int k = 0; k < gc->count; k++) { free(gc->px[k]); gc->px[k] = NULL; }
            gc->count = 0;
            return 0;
        }
        gc->px[gc->count] = px;
        gc->w[gc->count] = w;
        gc->h[gc->count] = h;
        gc->count++;
    }
    fclose(f);
    return gc->count > 0;
}

static void grid_cache_save_to_disk(const GridLibCache *gc, const char *view_id, int64_t count)
{
    if (gc->count <= 0) return;
    mkdir(GRID_CACHE_DIR, 0755);   /* ignore EEXIST/already-there */
    char path[300];
    grid_cache_disk_path(view_id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;

    int32_t n_covers = gc->count;
    fwrite(&count, sizeof(count), 1, f);
    fwrite(&n_covers, sizeof(n_covers), 1, f);
    for (int i = 0; i < gc->count; i++) {
        int32_t w = gc->w[i], h = gc->h[i];
        fwrite(&w, sizeof(w), 1, f);
        fwrite(&h, sizeof(h), 1, f);
        fwrite(gc->px[i], 1, (size_t)w * (size_t)h * 4, f);
    }
    fclose(f);
}

/* Bakes GRID_ALPHA into each cover's own pixels, once, instead of blending
 * it in on every redraw. Correct because draw_grid_background always draws
 * onto a backdrop just reset to pure black by fb_clear() — blending a pixel
 * at alpha a over black is exactly "pixel * a/255", nothing else, so
 * pre-scaling here and then drawing with a plain opaque copy (fb_blit_
 * opaque) produces an identical image for a fraction of the per-frame
 * CPU cost. Applied to the in-memory copy only — the on-disk cache (see
 * grid_cache_save_to_disk) keeps storing undimmed pixels, so a future
 * change to GRID_ALPHA doesn't need every disk cache entry invalidated. */
static void grid_dim_covers(GridLibCache *gc)
{
    for (int i = 0; i < gc->count; i++) {
        uint8_t *px = gc->px[i];
        size_t n = (size_t)gc->w[i] * (size_t)gc->h[i];
        for (size_t p = 0; p < n; p++) {
            uint8_t *pixel = px + p * 4;
            for (int c = 0; c < 3; c++) {
                uint32_t v = (uint32_t)pixel[c] * GRID_ALPHA;
                pixel[c] = (uint8_t)((v + (v >> 8) + 1) >> 8);
            }
        }
    }
}

/* Fills a freshly-reserved (memset to 0, view_id already set) cache slot —
 * disk cache first (grid_cache_load_from_disk), falling back to sequential
 * downloads+decodes (up to GRID_FETCH_MAX) otherwise. Shared by the
 * interactive path (grid_covers_sync, called from the main thread when the
 * user navigates to a library) and grid_prefetch_thread (background,
 * silent). dest_path is caller-owned scratch space (GRID_POSTER_TMP for the
 * main thread, GRID_POSTER_TMP_BG for the background thread) so the two
 * never clobber each other's in-flight download; show_ui is 0 for the
 * silent background path (no spinner/fb_flip — those must stay main-
 * thread-only, same reasoning as every other "don't touch the framebuffer
 * off-thread" rule already in this codebase). Deliberately NOT called
 * under g_grid_mutex — this does slow network I/O, and the slot it's
 * writing into isn't visible to any reader until the caller marks it
 * active/counted, so nothing needs the lock held here. */
static void grid_cache_populate(GridLibCache *gc, const JfItem *view,
                                 const char *dest_path, FBDev *fb, int show_ui)
{
    const char *item_type = collection_item_type(view->collection_type);
    /* Music gets square cells, every other library (and the synthetic
     * Continue/Next Up rows, mixed movies and episodes, never music) keeps
     * the original ~2:3 portrait cell posters already fit. See
     * draw_grid_background for how gc->square turns into an actual pixel
     * height — deliberately NOT derived here by picking a row count and
     * dividing fb->height by it (an integer row count this small can only
     * land near the target aspect, not on it — confirmed as the cause of a
     * visible ~10% distortion in both directions). gc->rows here is just
     * "enough rows to cover the screen plus one to spare", not tied to the
     * aspect math at all — see draw_grid_background, which lets the last
     * one clip off the bottom rather than stretching everything to fit
     * an exact count. */
    gc->square = !strcmp(view->collection_type, "music");
    {
        double target_ar = gc->square ? 1.0 : (2.0 / 3.0);
        int    cell_w     = fb->width / GRID_COLS;
        double cell_h     = cell_w / (target_ar * par_correction(fb));
        if (cell_h < 1.0) cell_h = 1.0;
        /* Only the horizontal crawl needs a spare cell (for the wraparound
         * sliver at each edge, see draw_grid_background) — there's no
         * vertical scroll, so covering the height needs nothing more than
         * enough rows to reach the bottom, +1 for float rounding safety.
         * Was +2, silently rendering a whole extra row nobody ever sees. */
        int rows = (int)ceil(fb->height / cell_h) + 1;
        if (rows > GRID_ROWS_MAX) rows = GRID_ROWS_MAX;
        gc->rows = rows;
    }
    JfItem grid_items[GRID_FETCH_MAX];
    int n;

    if (view_is_synthetic(view)) {
        /* No ParentId to list under — the covers are just the row's own
         * items. Deliberately not disk-cached either: the whole point of
         * these rows is that they change as things get watched, so a cache
         * keyed on a total that barely moves would show stale art for ages
         * (see grid_cache_load_from_disk's own staleness check). They're only
         * ever a couple of items, so re-fetching is cheap. */
        n = view_is_resume(view)
              ? jf_list_resume(s_cfg, grid_items, GRID_FETCH_MAX, NULL)
              : jf_list_nextup(s_cfg, grid_items, GRID_FETCH_MAX, NULL);
    } else {
        if (grid_cache_load_from_disk(gc, view, item_type)) {
            grid_cell_order_shuffle(gc);
            grid_dim_covers(gc);
            /* Release: everything written above must be visible to any
             * thread that observes ready == 1. See the acquire in
             * draw_grid_background. */
            __atomic_store_n(&gc->ready, 1, __ATOMIC_RELEASE);
            return;
        }
        n = jf_list_items_recursive(s_cfg, view->id, item_type, grid_items, GRID_FETCH_MAX);
    }

    int spinner_frame = 0;
    for (int i = 0; i < n && gc->count < GRID_FETCH_MAX; i++) {
        if (!grid_items[i].image_tag[0]) continue;
        if (show_ui) { draw_spinner_frame(fb, spinner_frame++); fb_flip(fb); }
        if (jf_download_item_image(s_cfg, grid_items[i].image_item_id, "Primary",
                                    grid_items[i].image_tag, 100, dest_path)) {
            uint8_t *px = load_image_tmp(dest_path, &gc->w[gc->count], &gc->h[gc->count]);
            if (px) gc->px[gc->count++] = px;
        }
    }
    grid_cell_order_shuffle(gc);
    if (!view_is_synthetic(view)) {
        int64_t count = jf_count_items(s_cfg, view->id, item_type);
        if (count >= 0) grid_cache_save_to_disk(gc, view->id, count);
    }
    grid_dim_covers(gc);
    __atomic_store_n(&gc->ready, 1, __ATOMIC_RELEASE);
}

/* Already-cached libraries (checked here under g_grid_mutex) return
 * immediately — just a linear scan over however many are cached, at most
 * GRID_LIB_CACHE_MAX. A never-seen library reserves its slot under the
 * lock (cheap) then populates it lock-free (grid_cache_populate does the
 * slow network I/O) before marking it active. */
void grid_covers_sync(FBDev *fb, const JfItem *view)
{
    pthread_mutex_lock(&g_grid_mutex);
    for (int i = 0; i < g_grid_cache_n; i++) {
        if (!strcmp(g_grid_cache[i].view_id, view->id)) {
            g_grid_active = i;
            pthread_mutex_unlock(&g_grid_mutex);
            return;
        }
    }
    if (g_grid_cache_n >= GRID_LIB_CACHE_MAX) {
        g_grid_active = -1;
        pthread_mutex_unlock(&g_grid_mutex);
        return;
    }
    int slot = g_grid_cache_n++;
    GridLibCache *gc = &g_grid_cache[slot];
    memset(gc, 0, sizeof(*gc));
    strncpy(gc->view_id, view->id, sizeof(gc->view_id) - 1);
    pthread_mutex_unlock(&g_grid_mutex);

    grid_cache_populate(gc, view, GRID_POSTER_TMP, fb, 1);

    pthread_mutex_lock(&g_grid_mutex);
    g_grid_active = slot;
    pthread_mutex_unlock(&g_grid_mutex);
}

/* Runs once, kicked off (detached) right after the home screen first
 * loads. Silently walks every library the user has and pre-populates any
 * that the interactive path hasn't already claimed, so switching to a
 * library the user hasn't visited yet in this session still shows its
 * mosaic immediately instead of a blank/dim background while it fetches.
 * Uses its own download scratch path and passes show_ui=0 so it never
 * draws — arg is still the real FBDev*, needed for its geometry (the
 * square-cell row count above is derived from it), just never written to
 * off the main thread. Safe to read: it's a stack value in main() that
 * outlives every thread here, and only ever read (width/height/geometry),
 * never mutated, off the main thread. */
static void *grid_prefetch_thread(void *arg)
{
    FBDev *fb = (FBDev *)arg;
    JfItem views[GRID_LIB_CACHE_MAX];
    int n = jf_list_views(s_cfg, views, GRID_LIB_CACHE_MAX);

    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&g_grid_mutex);
        int already = 0;
        for (int j = 0; j < g_grid_cache_n; j++)
            if (!strcmp(g_grid_cache[j].view_id, views[i].id)) { already = 1; break; }
        int slot = -1;
        if (!already && g_grid_cache_n < GRID_LIB_CACHE_MAX) {
            slot = g_grid_cache_n++;
            GridLibCache *gc = &g_grid_cache[slot];
            memset(gc, 0, sizeof(*gc));
            strncpy(gc->view_id, views[i].id, sizeof(gc->view_id) - 1);
        }
        pthread_mutex_unlock(&g_grid_mutex);
        if (slot < 0) continue;

        grid_cache_populate(&g_grid_cache[slot], &views[i], GRID_POSTER_TMP_BG, fb, 0);
    }
    return NULL;
}

void start_grid_prefetch(FBDev *fb)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, grid_prefetch_thread, fb);
    pthread_attr_destroy(&attr);
}

void draw_grid_background(FBDev *fb)
{
    if (g_grid_active < 0) return;
    GridLibCache *gc = &g_grid_cache[g_grid_active];
    if (!__atomic_load_n(&gc->ready, __ATOMIC_ACQUIRE) || gc->count == 0) return;
    int cell_w = fb->width / GRID_COLS;
    /* Same target_ar/cell_h derivation as grid_cache_populate (that
     * comment explains why it's computed this way, not by dividing
     * fb->height by an integer row count) — gc->rows there is just a loop
     * bound, generous enough that the last row(s) simply clip off the
     * bottom of the screen. */
    double target_ar = gc->square ? 1.0 : (2.0 / 3.0);
    int cell_h = (int)(cell_w / (target_ar * par_correction(fb)) + 0.5);
    if (cell_h < 1) cell_h = 1;

    /* Slow infinite horizontal crawl, alternating direction row to row —
     * per user request, kept gentle (GRID_SCROLL_PX_PER_SEC's own comment).
     * Each row cycles through its own GRID_COLS covers (cell_order's
     * width), and draws one extra cell past each edge so the wrap into the
     * next repeat is seamless as it scrolls off.
     *
     * `base` is how many whole cells have scrolled by so far — without it,
     * a column's logical index was pinned at "col % GRID_COLS" forever, so
     * each screen position only ever alternated between its two immediate
     * neighbors (whichever pair fmod's cell_w-periodic shift landed on),
     * bouncing back and forth every ~cell_w/GRID_SCROLL_PX_PER_SEC seconds
     * instead of actually advancing through all GRID_COLS covers — visible
     * as covers seeming to randomly swap back and forth. Folding `base`
     * into the logical column makes the full cycle GRID_COLS cells long,
     * as intended. */
    double scroll = now_sec() * GRID_SCROLL_PX_PER_SEC;
    for (int row = 0; row < gc->rows; row++) {
        double dir        = (row % 2 == 0) ? 1.0 : -1.0;
        double cell_units = dir * scroll / cell_w;
        double base_d     = floor(cell_units);
        int    base        = (int)base_d;
        double shift_f     = (cell_units - base_d) * cell_w;
        int    shift        = (int)shift_f;
        for (int col = -1; col <= GRID_COLS; col++) {
            int logical = col + base;
            int wrapped = ((logical % GRID_COLS) + GRID_COLS) % GRID_COLS;
            int idx = gc->cell_order[row * GRID_COLS + wrapped];
            fb_blit_opaque(fb, gc->px[idx], gc->w[idx], gc->h[idx],
                           col * cell_w - shift, row * cell_h, cell_w, cell_h);
        }

        /* Subpixel smoothing: at GRID_SCROLL_PX_PER_SEC=10 and ~50fps the
         * ideal motion is only 0.2px per frame, so drawing on whole pixels
         * means four identical frames then a 1px jump — the last visible
         * stepping after everything else was already paced to real vsync.
         * The ideal image is this row shifted left by the fractional
         * remainder f, and since neighboring screen pixels of the
         * integer-shifted rendering are exactly the two samples that
         * fraction sits between (including across cell seams — the next
         * cover is already drawn in place), a single horizontal 2-tap blend
         * over the finished row is all it takes: out = in[x]*(1-f) +
         * in[x+1]*f. R and B ride in one multiply via the usual 0xFF00FF
         * packing (weights sum to 256, so it can't carry across channels).
         * The final column clamps to itself — one screen-edge pixel, and
         * only when f != 0. */
        uint32_t f8 = (uint32_t)((shift_f - shift) * 256.0);
        if (f8) {
            uint32_t nf8 = 256 - f8;
            int y0 = row * cell_h;
            int y1 = y0 + cell_h;
            if (y1 > fb->height) y1 = fb->height;
            for (int y = y0; y < y1; y++) {
                uint32_t *px = (uint32_t *)(fb->back + (size_t)y * fb->stride);
                int last = fb->width - 1;
                for (int x = 0; x < last; x++) {
                    uint32_t a = px[x], b = px[x + 1];
                    uint32_t rb = (((a & 0xFF00FF) * nf8 + (b & 0xFF00FF) * f8) >> 8) & 0xFF00FF;
                    uint32_t g  = (((a & 0xFF00)   * nf8 + (b & 0xFF00)   * f8) >> 8) & 0xFF00;
                    px[x] = rb | g;
                }
            }
        }
    }
}

void draw_grid_gradient(FBDev *fb)
{
    /* Specialized inline version of the 288 separate fb_fill_rect_alpha(...,
     * 0, 0, 0, a) calls this used to be, one per pixel row. Solid black
     * (r=g=b=0) drops the (r*a)/(g*a)/(b*a) terms from the blend entirely —
     * each channel is just "darken by ia/255" — and doing the whole thing
     * in one pass avoids 288 redundant per-call bounds clamps and argument
     * setups. Measured on hardware (DEBUGLOG's "browse redraw rate" line)
     * as the single biggest contributor to the home carousel's ~17fps
     * ceiling — same visual output, just cheaper to produce it. */
    int h = fb->height;
    for (int y = 0; y < h; y++) {
        uint32_t ia = 255 - (uint32_t)(255 * y / (h - 1));
        uint32_t *row = (uint32_t *)(fb->back + (size_t)y * fb->stride);
        for (int x = 0; x < fb->width; x++) {
            uint32_t dst   = row[x];
            uint32_t out_r = ((dst >> 16) & 0xFF) * ia >> 8;
            uint32_t out_g = ((dst >>  8) & 0xFF) * ia >> 8;
            uint32_t out_b = ( dst        & 0xFF) * ia >> 8;
            row[x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}
