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

    fb->fd        = -1;
    fb->width     = w;
    fb->height    = h;
    fb->stride    = w * 4;
    fb->n_pages   = 1;
    fb->mmap_size = (size_t)fb->stride * fb->height;
    fb->headless  = 1;

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
    /* Detect double buffering: fbdev reports yres_virtual = 2*yres when
       mplayer's fbdev driver can page-flip via FBIOPAN_DISPLAY */
    fb->n_pages = 1;  /* always single — writing to page 1 corrupts mplayer's back buffer */
    fb->mmap_size = (size_t)fb->stride * fb->height;

    size_t size = fb->mmap_size;
    fb->mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fb->fd);
        return -1;
    }

    fb->back = (uint8_t *)calloc(1, size);
    if (!fb->back) {
        perror("alloc back-buffer");
        munmap(fb->mem, size);
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

void fb_flip(FBDev *fb)
{
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
    memcpy(fb->mem, fb->back, (size_t)fb->stride * fb->height);
    if (fb->headless) fb_dump_frame(fb);
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

    for (int fy = y0; fy < y1; fy++) {
        int sy = (fy - dy) * sh / dh;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;

        const uint8_t *src_row = pixels + sy * sw * 4;
        uint32_t      *dst_row = (uint32_t *)(fb->back + fy * fb->stride);

        for (int fx = x0; fx < x1; fx++) {
            int sx = (fx - dx) * sw / dw;
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;

            const uint8_t *src = src_row + sx * 4;
            uint32_t a = (uint32_t)src[3] * layer_alpha / 255;
            if (a == 0) continue;

            uint32_t dst  = dst_row[fx];
            uint32_t out_r = (src[0] * a + ((dst >> 16) & 0xFF) * (255 - a)) >> 8;
            uint32_t out_g = (src[1] * a + ((dst >>  8) & 0xFF) * (255 - a)) >> 8;
            uint32_t out_b = (src[2] * a + ( dst        & 0xFF) * (255 - a)) >> 8;

            dst_row[fx] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}
