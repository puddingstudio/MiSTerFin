/* padmap — decoding MiSTer's controller map file. See padmap.h for why.
 *
 * Pure: no evdev, no file I/O, no globals. input.c does the reading and the
 * device plumbing; everything here is exercised off-hardware by
 * tests/test_padmap.c against a real map file's bytes. */

#include <stdio.h>

#include "padmap.h"

/* Slot numbers, from Main_MiSTer/input.h. Only the ones MiSTerFin has a verb
 * for are listed — the file has 32, most of which are cores' business (X/Y,
 * paddles, the analog axis assignments) or MiSTer's own (the guide/guide2
 * OSD chord in 21/22). */
#define SLOT_RIGHT      0
#define SLOT_LEFT       1
#define SLOT_DOWN       2
#define SLOT_UP         3
#define SLOT_A          4
#define SLOT_B          5
#define SLOT_L          8
#define SLOT_R          9
#define SLOT_SELECT    10
#define SLOT_START     11
/* Both "menuok" and "menuesc" resolve here, packed into one slot's halves —
 * low 16 bits menuok, high 16 bits menuesc. See find_mister_button_num() in
 * Main_MiSTer/gamecontroller_db.cpp. */
#define SLOT_MENU_FUNC 23

static unsigned int slot_u32(const unsigned char *buf, int slot)
{
    const unsigned char *p = buf + slot * 4;
    return (unsigned int)p[0]        | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* One raw slot value -> a binding. Values at or above PADMAP_KEY_EMU are an
 * axis edge rather than a key; see the encoding note in padmap.h. */
static PadBind decode(unsigned int raw)
{
    PadBind b = { PADBIND_NONE, 0, 0 };
    if (!raw) return b;

    if (raw >= PADMAP_KEY_EMU) {
        unsigned int rel = raw - PADMAP_KEY_EMU;
        b.kind   = PADBIND_AXIS;
        b.code   = (unsigned short)(rel >> 1);
        b.at_max = (unsigned char)(rel & 1u);
    } else {
        b.kind = PADBIND_KEY;
        b.code = (unsigned short)raw;
    }
    return b;
}

int padmap_parse(const unsigned char *buf, int len, PadMap *out)
{
    if (!buf || !out || len != PADMAP_FILE_SIZE) return 0;

    static const struct { int verb, slot; } direct[] = {
        { PADV_RIGHT,  SLOT_RIGHT  }, { PADV_LEFT,   SLOT_LEFT   },
        { PADV_DOWN,   SLOT_DOWN   }, { PADV_UP,     SLOT_UP     },
        { PADV_L,      SLOT_L      }, { PADV_R,      SLOT_R      },
        { PADV_SELECT, SLOT_SELECT }, { PADV_START,  SLOT_START  },
    };

    for (int i = 0; i < PADV_COUNT; i++) {
        PadBind none = { PADBIND_NONE, 0, 0 };
        out->v[i] = none;
    }
    for (size_t i = 0; i < sizeof(direct) / sizeof(direct[0]); i++)
        out->v[direct[i].verb] = decode(slot_u32(buf, direct[i].slot));

    /* Confirm/back prefer the explicit menu pair over the core-facing A/B
     * slots: a pad can sensibly bind its cores' "A" to something that would
     * be a poor menu confirm, which is the whole reason MiSTer's db carries
     * menuok/menuesc separately. Absent (the common case for a pad mapped
     * before those keys existed), MiSTer's documented default is to fall back
     * to A and B — so that's what happens here. */
    unsigned int menu = slot_u32(buf, SLOT_MENU_FUNC);
    unsigned int ok   = menu & 0xFFFFu;
    unsigned int esc  = (menu >> 16) & 0xFFFFu;

    out->v[PADV_A] = decode(ok  ? ok  : slot_u32(buf, SLOT_A));
    out->v[PADV_B] = decode(esc ? esc : slot_u32(buf, SLOT_B));

    return 1;
}

int padmap_axis_pressed(int value, int min, int max, int at_max)
{
    /* A degenerate range would otherwise make every read look like a press
     * (and halve into a divide the caller can't see coming). An axis with no
     * travel can't be held. */
    if (max <= min) return 0;

    int centre = (min + max) / 2;
    if (at_max) return value >= centre + (max - centre) / 2;
    return value <= centre - (centre - min) / 2;
}

int padmap_filename(unsigned vid, unsigned pid, char *out, size_t len)
{
    if (!out || !len) return 0;
    int n = snprintf(out, len, "input_%04x_%04x_v3.map", vid & 0xFFFFu, pid & 0xFFFFu);
    if (n < 0 || (size_t)n >= len) { out[0] = 0; return 0; }
    return 1;
}
