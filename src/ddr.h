#pragma once
#include <stdint.h>

/* Zaparoo native-video DDR double-buffer writer (v2 protocol).
 *
 * WHY THIS EXISTS: MiSTerFin works on the standard menu core through the
 * ordinary framebuffer→scaler path — none of this file is needed for that.
 * But the Zaparoo Project (https://zaparoo.org) maintains a dual-mode fork
 * of the menu core (menu_zaparoo.rbf, feat/dual-mode-native-fb) that can
 * ALSO scan video out natively from DDR, and users who installed it get
 * double-buffered, structurally
 * tear-free playback with broadcast-style native scanout. That is a
 * trade-off rather than an upgrade over the standard path (which is
 * already tear-free in normal use via the mplayer vsync wait): the native
 * modes below scan out at 352 pixels wide, a downscale from the 640-wide
 * framebuffer. This file is MiSTerFin's side of that protocol, so the app
 * works on both cores and uses the native path when the Zaparoo core is
 * what's installed. Detection is main.c's content comparison of
 * /media/fat/menu.rbf against the installed Zaparoo copy; on a stock core
 * nothing here ever runs.
 *
 * The FPGA core reads frames from DDR instead of the framebuffer when the
 * control word contains the magic 0x5A50.  Without the Zaparoo menu.rbf
 * these writes are harmless (the core never looks at that region).
 *
 * DDR layout (ARM byte addresses):
 *   0x3A000000  control block (64-bit beat):
 *                 word0 [31:2]=frame_counter [0]=active_buf  (0 = stopped)
 *                 word1 [31:16]=0x5A50 [15:8]=h_off [7:4]=v_off [3:0]=mode
 *   0x3A001000  buffer 0  (v2)
 *   0x3A180000  buffer 1  (v2)
 *
 * Video modes:
 *   0  NTSC  352x240p60  stride=1408
 *   1  480i  720x480i60  stride=2880
 *   2  PAL   352x288p50  stride=1408  (default)
 */

int      ddr_init(void);            /* mmap /dev/mem; returns 0 on success */
void     ddr_close(void);           /* ddr_stop() + munmap */
int      ddr_ready(void);           /* non-zero after successful init */

void     ddr_set_mode(int mode);    /* 0=NTSC, 1=480i, 2=PAL (default) */
int      ddr_mode_width(void);
int      ddr_mode_height(void);
int      ddr_mode_stride(void);

uint8_t *ddr_back_buf(void);        /* pointer to inactive DDR buffer */

/* Copy from a wider source framebuffer (horizontal nearest-neighbor downscale).
 * src_stride: bytes per row in source (e.g. 640*4 = 2560 for standard fb0).
 * Copies exactly ddr_mode_width() pixels from the left edge of each row. */
void     ddr_copy_from_fb(const void *fb_mem, int fb_stride);

/* Publish the back buffer to the FPGA.  Write word1 then word0 as required
 * by the protocol (word1 must be stable before frame_counter changes). */
void     ddr_flip(int h_off, int v_off);

/* Zero the control block — FPGA reverts to standard framebuffer within one field. */
void     ddr_stop(void);
