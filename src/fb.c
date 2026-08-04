#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "fb.h"

/* MiSTer FPGA SPI vsync ---------------------------------------------------- */
#define FPGA_REG_BASE   0xFF000000u
#define FPGA_REG_SIZE   0x01000000u
#define SOCFPGA_MGR_OFF 0x706000u          /* 0xFF706000 - base */
#define GPO_OFF         (SOCFPGA_MGR_OFF + 0x10u)
#define GPI_OFF         (SOCFPGA_MGR_OFF + 0x14u)
#define SSPI_STROBE     (1u << 17)
#define SSPI_IO_EN      (1u << 20)
#define UIO_WAIT_VSYNC  0x30u

static volatile uint32_t *s_gpo;
static volatile uint32_t *s_gpi;

static void fpga_spi_init(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return;
    volatile uint32_t *base = (volatile uint32_t *)mmap(
        NULL, FPGA_REG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FPGA_REG_BASE);
    close(fd);
    if (base == (volatile uint32_t *)MAP_FAILED) return;
    s_gpo = base + (GPO_OFF >> 2);
    s_gpi = base + (GPI_OFF >> 2);
}

static void fpga_wait_vsync(void)
{
    if (!s_gpo) return;
    uint32_t gpo = *s_gpo | 0x80000000u | SSPI_IO_EN;
    *s_gpo = gpo;                                        /* EnableIO  */
    uint32_t g = (gpo & ~(0xFFFFu | SSPI_STROBE)) | UIO_WAIT_VSYNC;
    *s_gpo = g;
    *s_gpo = g | SSPI_STROBE;
    while (!(*s_gpi & SSPI_STROBE));                     /* wait ACK  */
    *s_gpo = g;
    while (*s_gpi & SSPI_STROBE);                        /* ACK clear */
    *s_gpo = gpo & ~SSPI_IO_EN;                          /* DisableIO */
}

/* MiSTer scaler framebuffer pages — see the matching PF_* block in
 * docker/vo_fbdev.c for the full story (the scaler latches the base
 * address only at vsync, so repointing it IS a hardware page flip; page 1
 * is the frame-sized slice right after the Linux fb in the driver's
 * region, matching vo_fbdev's pf_init). ONLY safe to call while
 * Main_MiSTer is SIGSTOPped (raw SPI would race its bus traffic
 * otherwise) — see pageflip_begin()/end() in main.c. */
#define UIO_SET_FBUF   0x2Fu
#define FBUF_FMT_WORD  0x8016u   /* FB_EN | FB_FMT_RxB | FB_FMT_8888 */

static uint32_t s_fb_page_phys[2];   /* set in fb_open from FSCREENINFO */
static int      s_reflip;            /* fb_flip re-asserts page 0, see fb_set_reflip */

static void fpga_spi_word(uint32_t base_gpo, uint16_t w)
{
    uint32_t g = (base_gpo & ~(0xFFFFu | SSPI_STROBE)) | w;
    *s_gpo = g;
    *s_gpo = g | SSPI_STROBE;
    while (!(*s_gpi & SSPI_STROBE));
    *s_gpo = g;
    while (*s_gpi & SSPI_STROBE);
}

/* While the video player owns the display (page flipping engaged), any UI
 * we draw lands in page 0 — but a late in-flight video frame can flip the
 * display to page 1 AFTER our one-time flip back (confirmed on hardware:
 * the pause overlay appeared and immediately vanished). With this on,
 * every fb_flip() re-asserts page 0, and since overlays are redrawn in the
 * ~100ms UI loop, the display self-heals within a tick. */
void fb_set_reflip(int on)
{
    s_reflip = on;
}

void fb_page_flip(int page)
{
    if (!s_gpo || !s_fb_page_phys[0]) return;
    uint32_t phys = s_fb_page_phys[page ? 1 : 0];
    uint32_t gpo  = (*s_gpo | 0x80000000u) | SSPI_IO_EN;
    *s_gpo = gpo;
    fpga_spi_word(gpo, UIO_SET_FBUF);
    fpga_spi_word(gpo, FBUF_FMT_WORD);
    fpga_spi_word(gpo, (uint16_t)(phys & 0xFFFF));
    /* Ending the command after the base words leaves the geometry
     * registers exactly as Main programmed them (positional protocol). */
    fpga_spi_word(gpo, (uint16_t)(phys >> 16));
    *s_gpo = gpo & ~SSPI_IO_EN;
}
/* -------------------------------------------------------------------------- */

/* Desktop/headless backend — fabricates an FBDev backed by plain malloc'd
 * buffers so the whole app can run without a real /dev/fb0 (which needs
 * fbdev ioctls a desktop Linux X/Wayland session doesn't provide). This is
 * exactly what run_preview_browse() in main.c used to hand-roll for itself;
 * having it here means the ordinary startup path works too, so the real UI
 * can be driven end-to-end off-hardware.
 *
 * Deliberately gated on MISTERFIN_FB being set rather than falling back
 * automatically when the real open fails: on the MiSTer a failed /dev/fb0
 * open is a genuine error worth reporting, and silently substituting an
 * invisible buffer would turn that into a mystery "app runs but nothing
 * appears" instead. */
static int fb_open_headless(FBDev *fb, const char *spec)
{
    int w = 0, h = 0;
    if (sscanf(spec, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
        fprintf(stderr, "MISTERFIN_FB: expected WxH (e.g. 640x288), got \"%s\"\n", spec);
        return -1;
    }

    fb->fd          = -1;
    fb->width       = w;
    fb->height      = h;
    fb->phys_height = h;
    fb->line_double = 0;
    fb->stride      = w * 4;
    fb->n_pages     = 1;
    fb->mmap_size   = (size_t)fb->stride * fb->height;
    fb->headless    = 1;

    fb->mem  = (uint8_t *)calloc(1, fb->mmap_size);
    fb->back = (uint8_t *)calloc(1, fb->mmap_size);
    if (!fb->mem || !fb->back) {
        free(fb->mem); free(fb->back);
        fb->mem = fb->back = NULL;
        perror("alloc headless framebuffer");
        return -1;
    }
    return 0;
}

/* Raw BGRX dump of the visible buffer, written on every fb_flip when
 * MISTERFIN_FRAME_OUT names a path. Always overwrites, so the file simply
 * holds the latest frame at all times — tools/raw_to_png.py turns it into
 * something viewable (stdlib zlib only, no image library needed). Headless
 * only: on real hardware this would be a per-frame disk write to the SD
 * card for no benefit. */
static void fb_dump_frame(const FBDev *fb)
{
    const char *out = getenv("MISTERFIN_FRAME_OUT");
    if (!out || !*out) return;
    FILE *f = fopen(out, "wb");
    if (!f) return;
    fwrite(fb->mem, 1, (size_t)fb->stride * fb->height, f);
    fclose(f);
}

int fb_open(FBDev *fb, const char *path)
{
    fb->fd       = -1;
    fb->mem      = NULL;
    fb->back     = NULL;
    fb->headless = 0;

    const char *headless_spec = getenv("MISTERFIN_FB");
    if (headless_spec && *headless_spec)
        return fb_open_headless(fb, headless_spec);

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) { perror("open framebuffer"); return -1; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl FBIOGET_*SCREENINFO");
        close(fb->fd);
        return -1;
    }

    fb->width  = (int)vinfo.xres;
    fb->height = (int)vinfo.yres;
    fb->stride = (int)finfo.line_length;
    /* Physical page addresses for fb_page_flip() — page 1 sits one frame
     * past the Linux fb, same layout vo_fbdev's pf_init establishes. */
    s_fb_page_phys[0] = (uint32_t)finfo.smem_start;
    s_fb_page_phys[1] = (uint32_t)finfo.smem_start + (uint32_t)finfo.line_length * vinfo.yres;
    /* Detect double buffering: fbdev reports yres_virtual = 2*yres when
       mplayer's fbdev driver can page-flip via FBIOPAN_DISPLAY */
    fb->n_pages = 1;  /* always single — writing to page 1 corrupts mplayer's back buffer */

    /* Interlaced full-frame raster (see fb.h): 576/480 lines are exactly
     * double the 288/240 layouts the UI is tuned for — halve the logical
     * height and let fb_flip line-double, instead of teaching every draw
     * call a second geometry. Only these two exact heights: anything else
     * is an unknown mode better rendered 1:1 than half-guessed. */
    fb->phys_height = fb->height;
    fb->line_double = 0;
    if (fb->height == 576 || fb->height == 480) {
        fb->line_double = 1;
        fb->height     /= 2;
    }
    fb->mmap_size = (size_t)fb->stride * fb->phys_height;

    fb->mem = mmap(NULL, fb->mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fb->fd);
        return -1;
    }

    fb->back = (uint8_t *)calloc(1, (size_t)fb->stride * fb->height);
    if (!fb->back) {
        perror("alloc back-buffer");
        munmap(fb->mem, fb->mmap_size);
        close(fb->fd);
        return -1;
    }

    fpga_spi_init();  /* open FPGA SPI registers for vsync wait */
    return 0;
}

void fb_close(FBDev *fb)
{
    if (fb->back) { free(fb->back); fb->back = NULL; }
    if (fb->mem && fb->mem != MAP_FAILED) {
        if (fb->headless) free(fb->mem);
        else              munmap(fb->mem, fb->mmap_size);
        fb->mem = NULL;
    }
    if (fb->fd >= 0) { close(fb->fd); fb->fd = -1; }
}

void fb_clear(FBDev *fb)
{
    memset(fb->back, 0, (size_t)fb->stride * fb->height);
}

unsigned long g_fb_flip_count = 0;

static FbOverlayFn s_overlay;

void fb_set_overlay(FbOverlayFn fn)
{
    s_overlay = fn;
}

void fb_flip(FBDev *fb)
{
    g_fb_flip_count++;

    /* See fb_set_overlay in fb.h. Runs before the copy so it lands in this
     * same frame; the guard makes a misbehaving overlay's nested fb_flip a
     * no-op instead of infinite recursion. */
    if (s_overlay) {
        static int in_overlay;
        if (!in_overlay) {
            in_overlay = 1;
            s_overlay(fb);
            in_overlay = 0;
        }
    }

    /* Wait for vsync BEFORE the copy so the write lands in the blanking
     * interval instead of racing the scan position — same fix already
     * proven in MiSTer-Toasty-Squadron's fb_flip(). Without this, nothing
     * in the whole UI (browse/info/pause/carousel/...) was gated on vsync
     * at all — only mplayer's own separately-patched vo_fbdev had its own
     * wait — so anything drawing multiple fb_flip()s in quick succession
     * (the carousel's slide animation, in particular) tore visibly. */
    if (!fb->headless) {
        uint32_t dummy = 0;
        ioctl(fb->fd, FBIO_WAITFORVSYNC, &dummy);
    }
    if (fb->line_double) {
        for (int y = 0; y < fb->height; y++) {
            const uint8_t *src = fb->back + (size_t)y * fb->stride;
            uint8_t       *dst = fb->mem + (size_t)(2 * y) * fb->stride;
            memcpy(dst, src, (size_t)fb->stride);
            memcpy(dst + fb->stride, src, (size_t)fb->stride);
        }
    } else {
        memcpy(fb->mem, fb->back, (size_t)fb->stride * fb->height);
    }
    if (s_reflip) fb_page_flip(0);   /* see fb_set_reflip */
    if (fb->headless) fb_dump_frame(fb);
}

void fb_sync_back(FBDev *fb)
{
    if (fb->line_double) {
        for (int y = 0; y < fb->height; y++)
            memcpy(fb->back + (size_t)y * fb->stride,
                   fb->mem + (size_t)(2 * y) * fb->stride, (size_t)fb->stride);
    } else {
        memcpy(fb->back, fb->mem, (size_t)fb->stride * fb->height);
    }
}

uint8_t *fb_mem_row(FBDev *fb, int y)
{
    return fb->mem + (size_t)(fb->line_double ? 2 * y : y) * fb->stride;
}

void fb_wait_vsync(FBDev *fb)
{
    if (fb->headless) return;   /* no display to sync to */
    uint32_t dummy = 0;
    ioctl(fb->fd, FBIO_WAITFORVSYNC, &dummy);
}

void fb_fill_rect_alpha(FBDev *fb,
                        int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (alpha == 0 || w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > fb->width  ? fb->width  : (x + w);
    int y1 = (y + h) > fb->height ? fb->height : (y + h);

    uint32_t a  = alpha;
    uint32_t ia = 255 - a;

    for (int fy = y0; fy < y1; fy++) {
        uint32_t *row = (uint32_t *)(fb->back + fy * fb->stride);
        for (int fx = x0; fx < x1; fx++) {
            uint32_t dst   = row[fx];
            uint32_t out_r = (r * a + ((dst >> 16) & 0xFF) * ia) >> 8;
            uint32_t out_g = (g * a + ((dst >>  8) & 0xFF) * ia) >> 8;
            uint32_t out_b = (b * a + ( dst        & 0xFF) * ia) >> 8;
            row[fx] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

void fb_blit(FBDev *fb,
             const uint8_t *pixels, int sw, int sh,
             int dx, int dy, int dw, int dh,
             uint8_t layer_alpha)
{
    if (!pixels || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    if (dx >= fb->width  || dx + dw <= 0) return;
    if (dy >= fb->height || dy + dh <= 0) return;

    int x0 = dx < 0 ? 0 : dx;
    int y0 = dy < 0 ? 0 : dy;
    int x1 = (dx + dw) > fb->width  ? fb->width  : (dx + dw);
    int y1 = (dy + dh) > fb->height ? fb->height : (dy + dh);

    /* sx depends only on fx, not fy, but was previously computed inside the
     * fy loop — recomputing the same values on every one of dh rows. On
     * this platform (Cortex-A9, no hardware integer divide — "/dw" is a
     * software routine) that redundant division was confirmed via DEBUGLOG
     * timing as the dominant cost of the grid mosaic background (~28ms of
     * a ~35ms frame). Precompute once; fb->width is 640 on every supported
     * mode (PAL 640x288 / NTSC 640x240), so a fixed-size row is enough. */
    int sx_row[640];
    int nx = x1 - x0;
    for (int i = 0; i < nx; i++) {
        int sx = (x0 + i - dx) * sw / dw;
        if (sx < 0) sx = 0;
        if (sx >= sw) sx = sw - 1;
        sx_row[i] = sx;
    }

    for (int fy = y0; fy < y1; fy++) {
        int sy = (fy - dy) * sh / dh;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;

        const uint8_t *src_row = pixels + sy * sw * 4;
        uint32_t      *dst_row = (uint32_t *)(fb->back + fy * fb->stride);

        for (int i = 0; i < nx; i++) {
            const uint8_t *src = src_row + sx_row[i] * 4;
            /* (v + (v>>8) + 1) >> 8: the standard fast /255 — exact for all
             * 0..65025 inputs, no software divide. */
            uint32_t av = (uint32_t)src[3] * layer_alpha;
            uint32_t a  = (av + (av >> 8) + 1) >> 8;
            if (a == 0) continue;

            uint32_t *dp   = dst_row + x0 + i;
            uint32_t dst   = *dp;
            uint32_t out_r = (src[0] * a + ((dst >> 16) & 0xFF) * (255 - a)) >> 8;
            uint32_t out_g = (src[1] * a + ((dst >>  8) & 0xFF) * (255 - a)) >> 8;
            uint32_t out_b = (src[2] * a + ( dst        & 0xFF) * (255 - a)) >> 8;

            *dp = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

void fb_blit_opaque(FBDev *fb,
                     const uint8_t *pixels, int sw, int sh,
                     int dx, int dy, int dw, int dh)
{
    if (!pixels || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    if (dx >= fb->width  || dx + dw <= 0) return;
    if (dy >= fb->height || dy + dh <= 0) return;

    int x0 = dx < 0 ? 0 : dx;
    int y0 = dy < 0 ? 0 : dy;
    int x1 = (dx + dw) > fb->width  ? fb->width  : (dx + dw);
    int y1 = (dy + dh) > fb->height ? fb->height : (dy + dh);

    int sx_row[640];
    int nx = x1 - x0;
    for (int i = 0; i < nx; i++) {
        int sx = (x0 + i - dx) * sw / dw;
        if (sx < 0) sx = 0;
        if (sx >= sw) sx = sw - 1;
        sx_row[i] = sx;
    }

    for (int fy = y0; fy < y1; fy++) {
        int sy = (fy - dy) * sh / dh;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;

        const uint8_t *src_row = pixels + sy * sw * 4;
        uint32_t      *dst_row = (uint32_t *)(fb->back + fy * fb->stride);

        for (int i = 0; i < nx; i++) {
            const uint8_t *src = src_row + sx_row[i] * 4;
            dst_row[x0 + i] = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
        }
    }
}
