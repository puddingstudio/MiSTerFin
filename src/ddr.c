#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include "ddr.h"

#define DDR_BASE       0x3A000000u
#define DDR_SIZE       0x00300000u   /* 3 MB mmap region */
#define DDR_BUF0_OFF   0x00001000u   /* v2 buffer 0 offset */
#define DDR_BUF1_OFF   0x00180000u   /* v2 buffer 1 offset */
#define DDR_MAGIC      0x5A50u       /* "ZP" — enables v2 mode */

static const int s_mode_w[]   = { 352, 720, 352 };
static const int s_mode_h[]   = { 240, 480, 288 };
static const int s_mode_str[] = { 1408, 2880, 1408 };

static volatile uint8_t *s_ddr    = NULL;
static uint32_t           s_frame = 0;
static int                s_active = 0;
static int                s_mode   = 2;  /* PAL 352x288 default */

int ddr_init(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -1;
    void *m = mmap(NULL, DDR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DDR_BASE);
    close(fd);
    if (m == MAP_FAILED) return -1;
    s_ddr = (volatile uint8_t *)m;
    /* Start in stopped state */
    *(volatile uint32_t *)(s_ddr + 0) = 0;
    *(volatile uint32_t *)(s_ddr + 4) = 0;
    s_frame  = 0;
    s_active = 0;
    return 0;
}

void ddr_close(void)
{
    if (!s_ddr) return;
    ddr_stop();
    munmap((void *)s_ddr, DDR_SIZE);
    s_ddr = NULL;
}

int ddr_ready(void) { return s_ddr != NULL; }

void ddr_set_mode(int mode)
{
    if (mode < 0 || mode > 2) mode = 2;
    s_mode = mode;
}

int ddr_mode_width(void)  { return s_mode_w[s_mode]; }
int ddr_mode_height(void) { return s_mode_h[s_mode]; }
int ddr_mode_stride(void) { return s_mode_str[s_mode]; }

uint8_t *ddr_back_buf(void)
{
    if (!s_ddr) return NULL;
    int next = s_active ^ 1;
    return (uint8_t *)(s_ddr + (next ? DDR_BUF1_OFF : DDR_BUF0_OFF));
}

void ddr_copy_from_fb(const void *fb_mem, int fb_stride)
{
    if (!s_ddr) return;
    uint8_t       *dst      = ddr_back_buf();
    const uint8_t *src      = (const uint8_t *)fb_mem;
    int            h        = s_mode_h[s_mode];
    int            ddr_str  = s_mode_str[s_mode];
    int            copy_w   = s_mode_w[s_mode];   /* pixels to copy per row */
    int            src_w    = fb_stride / 4;       /* source pixels per row  */

    if (src_w == copy_w) {
        /* Source and destination have the same width — fast path. */
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * ddr_str,
                   src + (size_t)y * fb_stride,
                   (size_t)ddr_str);
    } else {
        /* Source is wider (e.g. 640) — horizontal nearest-neighbor downscale. */
        for (int y = 0; y < h; y++) {
            const uint32_t *row_src = (const uint32_t *)(src + (size_t)y * fb_stride);
                  uint32_t *row_dst = (uint32_t *)(dst + (size_t)y * ddr_str);
            for (int x = 0; x < copy_w; x++)
                row_dst[x] = row_src[(size_t)x * src_w / copy_w];
        }
    }
}

void ddr_flip(int h_off, int v_off)
{
    if (!s_ddr) return;
    int      next   = s_active ^ 1;
    uint32_t frame  = ++s_frame;
    if (frame == 0) frame = ++s_frame;  /* never publish 0 (= stopped signal) */

    /* word1: magic | h_offset | v_offset (4-bit) | mode
     * Write word1 BEFORE word0 so the FPGA never sees a new frame_counter
     * with stale word1. */
    uint32_t word1 = ((uint32_t)DDR_MAGIC << 16)
                   | ((uint32_t)(uint8_t)(int8_t)h_off << 8)
                   | ((uint32_t)((uint8_t)(int8_t)v_off & 0x0Fu) << 4)
                   | ((uint32_t)(s_mode & 0x0Fu));

    __sync_synchronize();
    *(volatile uint32_t *)(s_ddr + 4) = word1;
    *(volatile uint32_t *)(s_ddr + 0) = (frame << 2) | (uint32_t)next;
    s_active = next;
}

void ddr_stop(void)
{
    if (!s_ddr) return;
    *(volatile uint32_t *)(s_ddr + 0) = 0;
    *(volatile uint32_t *)(s_ddr + 4) = 0;
    s_frame  = 0;
    s_active = 0;
}
