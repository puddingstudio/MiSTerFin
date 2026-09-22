/* padmap — parsing MiSTer's own controller map file.
 *
 * MiSTer's OSD "Define buttons" writes a per-VID/PID binary map under
 * /media/fat/config/inputs/. Reading it is how MiSTerFin stops guessing which
 * evdev code is which button: the file already says, for the exact pad the
 * user is holding, resolved through MiSTer's gamecontrollerdb handling and
 * whatever the user corrected by hand on top of that.
 *
 * The fixture below is not synthetic — it is the real 128 bytes off the test
 * box (input_045e_028e_v3.map, an 8BitDo M30 in X-input mode), the pad whose
 * R shoulder MiSTerFin could not see. Its D-pad is on ABS_X/ABS_Y rather than
 * the hat, and R is an analog trigger edge (ABS_Z at maximum) rather than a
 * button, which is exactly why a fixed BTN_* table missed both. */

#include <stdio.h>
#include <string.h>

#include "padmap.h"

/* evdev codes, spelled out so this test needs no linux headers. */
#define BTN_SOUTH_  304
#define BTN_EAST_   305
#define BTN_WEST_   308
#define BTN_TL_     310
#define BTN_TR_     311
#define BTN_SELECT_ 314
#define BTN_START_  315
#define BTN_MODE_   316
#define ABS_X_        0
#define ABS_Y_        1
#define ABS_Z_        2
#define ABS_RZ_       5

/* input_045e_028e_v3.map, byte for byte, from 192.168.20.81 on 2026-09-09. */
static const unsigned char m30_map[PADMAP_FILE_SIZE] = {
    0x01,0x03,0x00,0x00, 0x00,0x03,0x00,0x00, 0x03,0x03,0x00,0x00, 0x02,0x03,0x00,0x00,
    0x0b,0x03,0x00,0x00, 0x31,0x01,0x00,0x00, 0x37,0x01,0x00,0x00, 0x34,0x01,0x00,0x00,
    0x36,0x01,0x00,0x00, 0x05,0x03,0x00,0x00, 0x3a,0x01,0x00,0x00, 0x3b,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x3c,0x01,0x00,0x00, 0x3c,0x01,0x00,0x00, 0x31,0x01,0x0b,0x03,
    0x00,0x00,0x02,0x00, 0x01,0x00,0x02,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
};

static int checks = 0, fails = 0;

static const char *verb_name(int v)
{
    static const char *n[PADV_COUNT] = { "UP","DOWN","LEFT","RIGHT","A",
                                         "B","L","R","SELECT","START" };
    return (v >= 0 && v < PADV_COUNT) ? n[v] : "?";
}

static void want_key(const PadMap *m, int verb, int code)
{
    checks++;
    const PadBind *b = &m->v[verb];
    if (b->kind != PADBIND_KEY || b->code != code) {
        fails++;
        printf("  FAIL %s: want key %d, got kind=%d code=%d at_max=%d\n",
               verb_name(verb), code, b->kind, b->code, b->at_max);
    }
}

static void want_axis(const PadMap *m, int verb, int axis, int at_max)
{
    checks++;
    const PadBind *b = &m->v[verb];
    if (b->kind != PADBIND_AXIS || b->code != axis || b->at_max != at_max) {
        fails++;
        printf("  FAIL %s: want axis %d at_max=%d, got kind=%d code=%d at_max=%d\n",
               verb_name(verb), axis, at_max, b->kind, b->code, b->at_max);
    }
}

static void want_int(const char *label, int got, int expect)
{
    checks++;
    if (got != expect) {
        fails++;
        printf("  FAIL %s: got %d, want %d\n", label, got, expect);
    }
}

int main(void)
{
    PadMap m;

    /* ── the real pad ──────────────────────────────────────────────────── */
    want_int("parse accepts a 128-byte map",
             padmap_parse(m30_map, sizeof(m30_map), &m), 1);

    /* The bug that started this: R is an analog trigger edge, not a button.
     * A BTN_* lookup table cannot represent this binding at all. */
    want_axis(&m, PADV_R, ABS_Z_, 1);
    want_key(&m, PADV_L, BTN_TL_);

    /* This pad's D-pad reports on the stick axes; its ABS_HAT0X/Y exist but
     * stay at 0 through a press, which is why hat-only reading saw nothing. */
    want_axis(&m, PADV_RIGHT, ABS_X_, 1);
    want_axis(&m, PADV_LEFT,  ABS_X_, 0);
    want_axis(&m, PADV_DOWN,  ABS_Y_, 1);
    want_axis(&m, PADV_UP,    ABS_Y_, 0);

    /* Confirm/back come from slot 23's two halves (menuok low, menuesc high),
     * NOT from slots 4/5 — those are MiSTer's "A"/"B" for cores. Here menuok
     * is BTN_EAST, which agrees with what input.c already did by hand. */
    want_key(&m,  PADV_A, BTN_EAST_);
    want_axis(&m, PADV_B, ABS_RZ_, 1);

    want_key(&m, PADV_SELECT, BTN_SELECT_);
    want_key(&m, PADV_START,  BTN_START_);

    /* ── menuok/menuesc absent: fall back to slots 4/5 ─────────────────── */
    {
        unsigned char buf[PADMAP_FILE_SIZE];
        memcpy(buf, m30_map, sizeof(buf));
        memset(buf + 23 * 4, 0, 4);              /* clear MENU_FUNC */
        want_int("parse accepts the fallback map",
                 padmap_parse(buf, sizeof(buf), &m), 1);
        want_axis(&m, PADV_A, ABS_RZ_, 1);       /* slot 4, SYS_BTN_A */
        want_key(&m,  PADV_B, BTN_EAST_);        /* slot 5, SYS_BTN_B */
    }

    /* ── unmapped slots stay unbound ───────────────────────────────────── */
    {
        unsigned char buf[PADMAP_FILE_SIZE];
        memset(buf, 0, sizeof(buf));
        want_int("an all-zero map still parses", padmap_parse(buf, sizeof(buf), &m), 1);
        checks++;
        if (m.v[PADV_R].kind != PADBIND_NONE) {
            fails++;
            printf("  FAIL all-zero map: R should be unbound, got kind=%d\n",
                   m.v[PADV_R].kind);
        }
    }

    /* ── a truncated or oversized file is not a map ────────────────────── */
    want_int("a short buffer is rejected", padmap_parse(m30_map, 64, &m), 0);
    want_int("a NULL buffer is rejected",  padmap_parse(NULL, 128, &m), 0);

    /* ── axis edge thresholds ──────────────────────────────────────────── */
    /* A trigger: 0..255, released at 0, fully pressed at 255. */
    want_int("trigger released is not pressed", padmap_axis_pressed(0,   0, 255, 1), 0);
    want_int("trigger pressed",                 padmap_axis_pressed(255, 0, 255, 1), 1);
    want_int("trigger half-pull is not pressed",padmap_axis_pressed(100, 0, 255, 1), 0);

    /* A stick-reported D-pad: -32768..32767, resting near centre. The rest
     * position on this pad reads +-128, not 0, so a naive "non-zero means
     * pressed" test would fire constantly. */
    want_int("stick at rest is not pressed",  padmap_axis_pressed(128, -32768, 32767, 1), 0);
    want_int("stick at rest is not pressed(-)",padmap_axis_pressed(-128, -32768, 32767, 0), 0);
    want_int("stick pushed to max",           padmap_axis_pressed(32767, -32768, 32767, 1), 1);
    want_int("stick pushed to min",           padmap_axis_pressed(-32768, -32768, 32767, 0), 1);
    want_int("max-direction ignores min push",padmap_axis_pressed(-32768, -32768, 32767, 1), 0);

    /* A degenerate range must not divide by zero or report a phantom press. */
    want_int("zero-width axis is never pressed", padmap_axis_pressed(0, 0, 0, 1), 0);

    /* ── the filename MiSTer actually writes ───────────────────────────── */
    /* Lowercase, zero-padded to four digits each. Ground truth: the test box
     * holds input_045e_028e_v3.map for this pad. In menu context — which is
     * where MiSTerFin runs — the name carries no core-name prefix. */
    {
        char name[64];
        checks++;
        if (!padmap_filename(0x045e, 0x028e, name, sizeof(name)) ||
            strcmp(name, "input_045e_028e_v3.map") != 0) {
            fails++;
            printf("  FAIL filename: got \"%s\", want \"input_045e_028e_v3.map\"\n", name);
        }
        /* A vendor or product with a leading zero must keep all four digits,
         * or the lookup silently misses. */
        checks++;
        if (!padmap_filename(0x0079, 0x0006, name, sizeof(name)) ||
            strcmp(name, "input_0079_0006_v3.map") != 0) {
            fails++;
            printf("  FAIL filename padding: got \"%s\", "
                   "want \"input_0079_0006_v3.map\"\n", name);
        }
        want_int("filename rejects a too-small buffer",
                 padmap_filename(0x045e, 0x028e, name, 8), 0);
    }

    if (fails) {
        printf("padmap: %d checks, %d FAILURES\n", checks, fails);
        return 1;
    }
    printf("padmap: %d checks, 0 failures\n", checks);
    return 0;
}
