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

int fb_open(FBDev *fb, const char *path)
{
    fb->fd   = -1;
    fb->mem  = NULL;
    fb->back = NULL;

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
        munmap(fb->mem, fb->mmap_size);
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
    memcpy(fb->mem, fb->back, (size_t)fb->stride * fb->height);
}

void fb_wait_vsync(FBDev *fb)
{
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
