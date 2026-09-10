#ifndef PADMAP_H
#define PADMAP_H

/* MiSTer's own controller map, read rather than guessed.
 *
 * MiSTer's OSD "Define buttons" flow writes a small binary file per
 * vendor/product under /media/fat/config/inputs/ — in menu context (which is
 * where MiSTerFin runs) that's the un-prefixed "input_<vid>_<pid>_v3.map".
 * Main_MiSTer seeds it from its bundled gamecontrollerdb and the user can
 * correct it by hand, so by the time it lands on disk it is the resolved,
 * per-device truth for the exact pad in someone's hands.
 *
 * Reading it is what lets input.c stop maintaining a fixed BTN_* table that
 * only ever fit one shape of pad. Two things that table structurally could
 * not express, both confirmed on hardware with an 8BitDo M30 in X-input mode:
 * a button bound to an ANALOG TRIGGER edge (its R shoulder is ABS_Z at
 * maximum), and a D-pad reported on the stick axes rather than the hat.
 *
 * The parsing here is deliberately free of evdev and file I/O so it can be
 * tested off-hardware — see tests/test_padmap.c, whose fixture is a real map
 * file lifted byte for byte off the test box. */

#include <stddef.h>

/* 32 slots of 4 bytes. Main_MiSTer/input.h: NUMBUTTONS 32. */
#define PADMAP_FILE_SIZE 128

/* Main_MiSTer/input.h: KEY_EMU (KEY_MAX+1). A slot value at or above this is
 * not a key code but an axis edge, encoded as KEY_EMU + (axis << 1), with the
 * low bit set for the axis MAXIMUM and clear for its minimum. That single
 * encoding is how MiSTer binds triggers, sticks and hats uniformly. */
#define PADMAP_KEY_EMU   768

typedef enum {
    PADBIND_NONE = 0,   /* the user left this action unmapped */
    PADBIND_KEY,        /* code is an evdev EV_KEY code */
    PADBIND_AXIS        /* code is an EV_ABS axis; at_max picks the direction */
} PadBindKind;

typedef struct {
    unsigned char  kind;    /* PadBindKind */
    unsigned short code;
    unsigned char  at_max;  /* PADBIND_AXIS only: fires at the axis maximum */
} PadBind;

/* MiSTerFin's own verbs, which are a subset of MiSTer's 32 slots. Ordered
 * for readability, not to match the file — padmap_parse does that mapping. */
enum {
    PADV_UP, PADV_DOWN, PADV_LEFT, PADV_RIGHT,
    PADV_A, PADV_B, PADV_L, PADV_R, PADV_SELECT, PADV_START,
    PADV_COUNT
};

typedef struct { PadBind v[PADV_COUNT]; } PadMap;

/* Decodes a map file's bytes. Returns 1 on success, 0 if buf isn't exactly
 * PADMAP_FILE_SIZE bytes (anything else is not a v3 map and is ignored rather
 * than guessed at). Unmapped actions come back as PADBIND_NONE. */
int padmap_parse(const unsigned char *buf, int len, PadMap *out);

/* Whether an axis currently reads as "pressed" in the given direction.
 *
 * Needs a real threshold rather than a test against zero: a stick-reported
 * D-pad rests near the centre of its range but not exactly on it (the M30
 * rests at +-128 of a +-32768 range), so anything looser fires constantly.
 * The trip point sits halfway between the centre and the extreme, which suits
 * both a 0..255 trigger and a full-range stick. */
int padmap_axis_pressed(int value, int min, int max, int at_max);

/* Builds the map filename for a device's USB vendor/product, e.g.
 * "input_045e_028e_v3.map". Lowercase and zero-padded to four digits each —
 * get either wrong and the lookup just misses, with no visible error. This is
 * the MENU-context name (no core-name prefix), which is the one MiSTerFin
 * wants: see get_map_name()'s is_menu() branch in Main_MiSTer/input.cpp.
 * Returns 1 on success, 0 if out is too small. */
int padmap_filename(unsigned vid, unsigned pid, char *out, size_t len);

#endif
