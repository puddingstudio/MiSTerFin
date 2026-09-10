/* Input — evdev gamepads/keyboards, the desktop stdin backend, scripted
 * key playback, and navigation auto-repeat. Extracted verbatim from
 * main.c; see input.h for the interface. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <termios.h>
#include <linux/input.h>

#include "input.h"
#include "padmap.h"
#include "util.h"

/* ── input (evdev gamepad) ────────────────────────────────────────────────── */

/* Bit-array helpers for the evdev state ioctls below (linux/input.h returns
 * these as an array of unsigned long). */
#define INPUT_BITS_PER_LONG  (8 * (int)sizeof(unsigned long))
#define INPUT_NLONGS(n)      (((n) + INPUT_BITS_PER_LONG - 1) / INPUT_BITS_PER_LONG)
#define INPUT_TEST_BIT(arr, bit) \
    ((arr)[(bit) / INPUT_BITS_PER_LONG] & (1UL << ((bit) % INPUT_BITS_PER_LONG)))

/* Was 8. A MiSTer has well over that many /dev/input/event* nodes once
 * keyboards, mice, the MiSTer virtual input device and a pad or two are
 * present — and a single pad often exposes several. With a hard cap and no
 * eviction, which devices got opened came down to readdir order, which is
 * filesystem order rather than anything meaningful. */
#define MAX_INPUT_FDS 32
static int  input_fds[MAX_INPUT_FDS];
static int  input_swap_ab[MAX_INPUT_FDS];
static int  input_is_virtual[MAX_INPUT_FDS];
static char input_names[MAX_INPUT_FDS][32];   /* e.g. "event0" — see input_open() */
static char input_display_names[MAX_INPUT_FDS][128]; /* EVIOCGNAME, e.g. "Microsoft X-Box 360 pad" */
static int  input_count = 0;
/* MISTERFIN_INPUT_DEBUG=1 — prints every incoming event with its device, raw
 * code, and what it mapped to (or why it was dropped). MiSTer's input routing
 * is genuinely unusual: it grabs directly-wired USB pads exclusively and
 * re-emits them on a synthetic "MiSTer virtual input" device, so which button
 * arrives on which node, under which code, varies by pad and by connection
 * type. Guessing at that from a bug report is hopeless; this makes it a
 * two-minute answer. Output goes to stderr, so run over SSH — it doesn't
 * touch the framebuffer UI. */
static int  input_debug = 0;

/* ── MiSTer's own per-device map (see padmap.h) ──────────────────────────────
 * Where MiSTer has a mapping for a pad, it is authoritative and the fixed
 * BTN_* table below is not consulted for that device at all. That table can
 * only describe a pad whose buttons are buttons and whose D-pad is a hat;
 * measured on an 8BitDo M30 in X-input mode, neither holds — its D-pad
 * reports on ABS_X/ABS_Y and its R shoulder is an ABS_Z trigger edge, so two
 * of its inputs were unreachable no matter what codes the table listed. */
#define PADMAP_DIR "/media/fat/config/inputs"

typedef struct { int min, max, pressed; } AxisState;

static PadMap    input_map[MAX_INPUT_FDS];
static int       input_has_map[MAX_INPUT_FDS];
static AxisState input_axis[MAX_INPUT_FDS][PADV_COUNT];

/* Set once any real device has a map. That's the signal to stop trusting
 * MiSTer's virtual echo device: with no exclusive grab in play (there isn't
 * one — a plain reader sees the pad's raw events while MiSTer holds it open)
 * a single press arrives on BOTH the pad and the echo, and any button mapped
 * on both paths fires twice whenever the two land in different poll cycles.
 * One path per device is the fix; a wider allowlist would only add collisions. */
static int       input_mapped_any = 0;

/* PADV_* order -> INP_* bits. */
static const int padv_to_inp[PADV_COUNT] = {
    INP_UP, INP_DOWN, INP_LEFT, INP_RIGHT,
    INP_A,  INP_B,    INP_L,    INP_R,     INP_SELECT, INP_START
};


/* Some 8BitDo SNES-style pads (confirmed on the SFC30 via raw evdev capture)
 * report their printed A/B buttons as BTN_SOUTH/BTN_EAST swapped relative to
 * the usual position-based convention (printed A -> BTN_SOUTH, printed B ->
 * BTN_EAST) — the firmware enumerates buttons in legacy SNES ordinal order
 * rather than by physical/compass position, unlike XInput-style pads. Swap
 * them back per-device so A is always "confirm" and B is always "back". */
static int device_needs_ab_swap(const char *name)
{
    return strstr(name, "SFC30") != NULL;
}

/* MiSTer's own OSD layer echoes physical joystick presses as synthetic
 * keyboard events on a separate virtual device — its uinput device, named
 * here, created in Main_MiSTer's input.cpp. Raw evdev capture on hardware
 * (2026-09-09) shows it carrying the four arrows, Enter/Esc, and L/R as
 * KEY_PAGEUP/KEY_PAGEDOWN, but NOT Select or Start.
 *
 * It can't simply be closed. Some pads have no usable path of their own —
 * the SFC30's D-pad arrives only through this echo, having no EV_ABS
 * capability at all (confirmed via /proc/bus/input/devices) — so for a
 * device MiSTer has no map for, the echo is still the fallback.
 *
 * Where a pad DOES have a map, this device is ignored entirely instead:
 * both paths are live simultaneously (there is no grab), so trusting both
 * makes every press that exists on both fire twice. See input_mapped_any. */
static int device_is_mister_virtual(const char *name)
{
    return strcmp(name, "MiSTer virtual input") == 0;
}


static const char *inp_bit_name(int bit)
{
    switch (bit) {
    case INP_UP: return "UP";       case INP_DOWN:  return "DOWN";
    case INP_LEFT: return "LEFT";   case INP_RIGHT: return "RIGHT";
    case INP_A: return "A";         case INP_B:     return "B";
    case INP_START: return "START"; case INP_SELECT: return "SELECT";
    case INP_L: return "L";         case INP_R:      return "R";
    default: return "?";
    }
}

/* Safe to call repeatedly (see the periodic re-scan in the main loop) —
 * skips any /dev/input/eventN already tracked, only opening ones that are
 * new since the last call (a reconnected wireless pad, for example, can
 * come back as a fresh node with a different number). */
static void stdin_input_open(void);   /* forward decls — desktop backends, defined below */
static void script_open(void);
static void stdin_input_restore(void);
static void stdin_input_drain(void);

/* Loads MiSTer's map for whatever pad is on this fd, if it has one. Returns 1
 * when a map was found and parsed. A device with no map (never run through
 * "Define buttons", or a keyboard) keeps the legacy table — that fallback is
 * what makes this safe to ship without demanding setup from anyone. */
static int load_device_map(int slot, int fd)
{
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) return 0;

    char fname[64], path[128];
    if (!padmap_filename(id.vendor, id.product, fname, sizeof(fname))) return 0;
    snprintf(path, sizeof(path), "%s/%s", PADMAP_DIR, fname);

    int mfd = open(path, O_RDONLY | O_CLOEXEC);
    if (mfd < 0) {
        if (input_debug) fprintf(stderr, "[input]   no map at %s\n", path);
        return 0;
    }
    unsigned char buf[PADMAP_FILE_SIZE];
    ssize_t got = read(mfd, buf, sizeof(buf));
    close(mfd);

    if (got != (ssize_t)sizeof(buf) ||
        !padmap_parse(buf, (int)got, &input_map[slot])) {
        if (input_debug)
            fprintf(stderr, "[input]   %s is not a v3 map (%d bytes), ignoring\n",
                    path, (int)got);
        return 0;
    }

    /* Cache each axis binding's range now. An axis edge can only be turned
     * into a press with the range in hand, and asking once per press would
     * mean an ioctl inside the event loop. Seeding `pressed` from the current
     * value stops a trigger that is already held at startup from reading as a
     * fresh press on the first poll. */
    for (int v = 0; v < PADV_COUNT; v++) {
        AxisState *st = &input_axis[slot][v];
        st->min = st->max = st->pressed = 0;
        if (input_map[slot].v[v].kind != PADBIND_AXIS) continue;

        struct input_absinfo abs;
        if (ioctl(fd, EVIOCGABS(input_map[slot].v[v].code), &abs) < 0) continue;
        st->min = abs.minimum;
        st->max = abs.maximum;
        st->pressed = padmap_axis_pressed(abs.value, st->min, st->max,
                                          input_map[slot].v[v].at_max);
    }

    if (input_debug) {
        static const char *vn[PADV_COUNT] = { "UP","DOWN","LEFT","RIGHT","A",
                                              "B","L","R","SELECT","START" };
        fprintf(stderr, "[input]   map %s:\n", fname);
        for (int v = 0; v < PADV_COUNT; v++) {
            PadBind *b = &input_map[slot].v[v];
            if (b->kind == PADBIND_KEY)
                fprintf(stderr, "[input]     %-6s <- code %d\n", vn[v], b->code);
            else if (b->kind == PADBIND_AXIS)
                fprintf(stderr, "[input]     %-6s <- axis %d %s (range %d..%d)\n",
                        vn[v], b->code, b->at_max ? "max" : "min",
                        input_axis[slot][v].min, input_axis[slot][v].max);
            else
                fprintf(stderr, "[input]     %-6s <- unmapped\n", vn[v]);
        }
    }
    return 1;
}

void input_open(void)
{
    /* Desktop backends come up alongside (not instead of) evdev — both are
     * no-ops unless their own env var asked for them, so this stays exactly
     * the old behavior on real hardware. Safe to call repeatedly, same as
     * the evdev scan below (see this function's own header comment). */
    if (getenv("MISTERFIN_STDIN")) stdin_input_open();
    if (getenv("MISTERFIN_INPUT_DEBUG")) input_debug = 1;
    script_open();

    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && input_count < MAX_INPUT_FDS) {
        if (strncmp(e->d_name, "event", 5)) continue;

        int already = 0;
        for (int i = 0; i < input_count; i++)
            if (!strcmp(input_names[i], e->d_name)) { already = 1; break; }
        if (already) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* Skip anything with neither buttons nor axes. A system has plenty of
         * such nodes (power buttons, lid switches, accelerometers, the
         * console's own pseudo-devices) and every one of them used to occupy
         * a slot that a real controller then couldn't have. */
        unsigned long evbits[INPUT_NLONGS(EV_MAX + 1)];
        memset(evbits, 0, sizeof(evbits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
            (!INPUT_TEST_BIT(evbits, EV_KEY) && !INPUT_TEST_BIT(evbits, EV_ABS))) {
            close(fd);
            continue;
        }

        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        int slot = input_count;
        input_swap_ab[slot]    = device_needs_ab_swap(name);
        input_is_virtual[slot] = device_is_mister_virtual(name);
        strncpy(input_names[slot], e->d_name, sizeof(input_names[0]) - 1);
        strncpy(input_display_names[slot], name, sizeof(input_display_names[0]) - 1);
        input_fds[slot] = fd;
        input_count++;

        if (input_debug)
            fprintf(stderr, "[input] opened %s \"%s\"%s (%d tracked)\n",
                    e->d_name, name,
                    input_is_virtual[slot] ? " [MiSTer virtual]" : "",
                    input_count);

        /* The virtual echo is MiSTer's re-emission of a pad it already
         * mapped, so it has no map of its own to look up. */
        input_has_map[slot] = 0;
        if (!input_is_virtual[slot] && load_device_map(slot, fd)) {
            input_has_map[slot] = 1;
            input_mapped_any = 1;
        }
    }
    closedir(d);
}

static void input_repeat_reset(void);   /* forward decl — defined with the repeat logic below */

/* Drops a device that has disappeared, compacting the parallel arrays.
 *
 * Without this, a controller that disconnects and comes back — which is
 * routine for anything wireless, and is exactly what happens after a pad
 * freezes and gets reconnected — leaves its dead entry holding a slot
 * forever while the live node needs a new one. Slots leak on every reconnect
 * until nothing new can be opened at all, and buttons simply stop arriving
 * with no visible cause. */
static void input_drop_slot(int i)
{
    if (input_debug)
        fprintf(stderr, "[input] %s went away, releasing slot\n", input_names[i]);
    close(input_fds[i]);
    for (int j = i; j < input_count - 1; j++) {
        input_fds[j]        = input_fds[j + 1];
        input_swap_ab[j]    = input_swap_ab[j + 1];
        input_is_virtual[j] = input_is_virtual[j + 1];
        input_has_map[j]    = input_has_map[j + 1];
        input_map[j]        = input_map[j + 1];
        memcpy(input_axis[j], input_axis[j + 1], sizeof(input_axis[0]));
        memcpy(input_names[j], input_names[j + 1], sizeof(input_names[0]));
        memcpy(input_display_names[j], input_display_names[j + 1], sizeof(input_display_names[0]));
    }
    input_count--;

    /* Recomputed rather than left latched: unplugging the last mapped pad has
     * to hand the virtual echo back, or a user who pulls their controller is
     * left with an app that ignores every remaining input path. */
    input_mapped_any = 0;
    for (int j = 0; j < input_count; j++)
        if (input_has_map[j]) { input_mapped_any = 1; break; }
}

void input_drain(void)
{
    struct input_event ev;
    for (int i = 0; i < input_count; i++)
        while (read(input_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {}
    input_repeat_reset();
    stdin_input_drain();
}

void input_close(void)
{
    for (int i = 0; i < input_count; i++) close(input_fds[i]);
    input_count = 0;
    stdin_input_restore();
}

/* For a one-time startup log line (see jf_log_line callers in main.c) —
 * a static summary of what got opened, not a live per-event trace like
 * MISTERFIN_INPUT_DEBUG already provides. */
int input_device_count(void) { return input_count; }
const char *input_device_node(int i) { return input_names[i]; }
const char *input_device_name(int i) { return input_display_names[i]; }
int input_device_is_virtual(int i) { return input_is_virtual[i]; }

/* ── desktop keyboard backend (stdin, raw mode) ──────────────────────────
 * Off-hardware there are no /dev/input/eventN gamepads to read, so the same
 * INP_* masks are sourced from the controlling terminal instead. Enabled by
 * MISTERFIN_STDIN=1 (interactive) or implicitly by MISTERFIN_KEYS (scripted,
 * below) — never on the MiSTer, where the evdev path above is the real one.
 *
 * Terminals do their own key repeat when a key is held, so this backend gets
 * auto-repeat for free and doesn't participate in the evdev held-state
 * repeat logic. */
static int  stdin_enabled = 0;
static struct termios stdin_saved_termios;
static int  stdin_termios_saved = 0;

static void stdin_input_restore(void)
{
    if (!stdin_termios_saved) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_saved_termios);
    stdin_termios_saved = 0;
}

static void stdin_input_open(void)
{
    if (stdin_enabled || !isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &stdin_saved_termios) != 0) return;

    struct termios raw = stdin_saved_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);   /* byte-at-a-time, no local echo */
    raw.c_cc[VMIN]  = 0;                          /* fully non-blocking reads */
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;

    stdin_termios_saved = 1;
    stdin_enabled       = 1;
    atexit(stdin_input_restore);   /* also covers the on_fatal/_exit paths' terminal state */
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
}

/* Discards buffered keystrokes typed while a blocking operation was running,
 * matching what input_drain() does for the evdev queues. Scripted playback
 * is deliberately unaffected — it's paced off the wall clock rather than
 * buffered, so there's nothing to drop. */
static void stdin_input_drain(void)
{
    if (!stdin_enabled) return;
    unsigned char buf[64];
    while (read(STDIN_FILENO, buf, sizeof(buf)) > 0) {}
}

/* Maps one already-decoded terminal key to an INP_* bit. Escape sequences
 * (arrows, PageUp/Down, Home) are decoded by the caller and passed in as the
 * synthetic codes below, which are deliberately outside the ASCII range so
 * they can't collide with a real typed character. */
#define TK_UP     0x100
#define TK_DOWN   0x101
#define TK_LEFT   0x102
#define TK_RIGHT  0x103
#define TK_PGUP   0x104
#define TK_PGDN   0x105
#define TK_HOME   0x106
#define TK_ESC    0x107

static int stdin_key_to_mask(int key)
{
    switch (key) {
    case TK_UP:                 return INP_UP;
    case TK_DOWN:               return INP_DOWN;
    case TK_LEFT:               return INP_LEFT;
    case TK_RIGHT:              return INP_RIGHT;
    /* Same pairing the evdev path uses: Enter/X confirm, Esc/Z back. */
    case '\r': case '\n':
    case 'x': case 'X':         return INP_A;
    case TK_ESC:
    case 0x7f: case '\b':
    case 'z': case 'Z':         return INP_B;
    case '\t':                  return INP_SELECT;
    case TK_HOME: case 'p':     return INP_START;
    case TK_PGUP: case '[':     return INP_L;
    case TK_PGDN: case ']':     return INP_R;
    default:                    return 0;
    }
}

/* Decodes whatever bytes are pending on stdin into an INP_* mask. 'q' quits
 * outright — there's no window manager to close off-hardware, and Esc is
 * already spoken for as "back". */
static int stdin_poll(void)
{
    if (!stdin_enabled) return 0;

    unsigned char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return 0;

    int mask = 0;
    for (ssize_t i = 0; i < n; i++) {
        int key = buf[i];
        if (key == 'q') { g_running = 0; continue; }

        /* CSI sequences arrive as one contiguous burst in the same read, so
         * looking ahead within this buffer is enough — a bare ESC (nothing
         * following it) is a real Esc keypress. */
        if (key == 0x1b) {
            if (i + 2 < n && buf[i + 1] == '[') {
                unsigned char c = buf[i + 2];
                i += 2;
                switch (c) {
                case 'A': key = TK_UP;    break;
                case 'B': key = TK_DOWN;  break;
                case 'C': key = TK_RIGHT; break;
                case 'D': key = TK_LEFT;  break;
                case 'H': key = TK_HOME;  break;
                /* "5~"/"6~"/"1~" — swallow the trailing '~' too */
                case '5': key = TK_PGUP; if (i + 1 < n && buf[i+1] == '~') i++; break;
                case '6': key = TK_PGDN; if (i + 1 < n && buf[i+1] == '~') i++; break;
                case '1': key = TK_HOME; if (i + 1 < n && buf[i+1] == '~') i++; break;
                default:  continue;   /* some other CSI sequence — ignore it */
                }
            } else {
                key = TK_ESC;
            }
        }
        mask |= stdin_key_to_mask(key);
    }
    return mask;
}

/* ── scripted key playback (MISTERFIN_KEYS) ──────────────────────────────
 * A comma-separated list of key names, each optionally followed by ":<ms>"
 * for how long to wait after it before the next one (default SCRIPT_STEP_MS)
 * — e.g. MISTERFIN_KEYS="right,right,a:800,down,down".
 *
 * Reproducible screenshots without a human at the keyboard: pair it with
 * MISTERFIN_FRAME_OUT and the raw file holds whatever was on screen when the
 * script finished. The app quits once the queue drains (that's the point —
 * it's for automation), unless MISTERFIN_KEYS_HOLD=1 asks it to stay up. */
#define SCRIPT_MAX      256
#define SCRIPT_STEP_MS  400

typedef struct { int mask; int delay_ms; } ScriptKey;
static ScriptKey script_keys[SCRIPT_MAX];
static int    script_count = 0, script_pos = 0;
static double script_next_at = 0.0;
static int    script_hold = 0;

static int script_name_to_mask(const char *name)
{
    if (!strcmp(name, "up"))     return INP_UP;
    if (!strcmp(name, "down"))   return INP_DOWN;
    if (!strcmp(name, "left"))   return INP_LEFT;
    if (!strcmp(name, "right"))  return INP_RIGHT;
    if (!strcmp(name, "a"))      return INP_A;
    if (!strcmp(name, "b"))      return INP_B;
    if (!strcmp(name, "select")) return INP_SELECT;
    if (!strcmp(name, "start"))  return INP_START;
    if (!strcmp(name, "l"))      return INP_L;
    if (!strcmp(name, "r"))      return INP_R;
    if (!strcmp(name, "wait"))   return 0;   /* pure delay, no button */
    fprintf(stderr, "MISTERFIN_KEYS: unknown key \"%s\"\n", name);
    return 0;
}

/* input_open() re-runs every few seconds to pick up hotplugged pads, so this
 * has to be idempotent — without the latch, each rescan would re-parse the
 * script and rewind it to the start, looping forever. */
static void script_open(void)
{
    static int script_loaded = 0;
    if (script_loaded) return;
    script_loaded = 1;

    const char *spec = getenv("MISTERFIN_KEYS");
    if (!spec || !*spec) return;
    const char *hold = getenv("MISTERFIN_KEYS_HOLD");
    script_hold = (hold && *hold && strcmp(hold, "0") != 0);

    char list[1024];
    strncpy(list, spec, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';

    for (char *tok = strtok(list, ","); tok && script_count < SCRIPT_MAX;
         tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        int delay = SCRIPT_STEP_MS;
        char *colon = strchr(tok, ':');
        if (colon) { *colon = '\0'; delay = atoi(colon + 1); }
        script_keys[script_count].mask     = script_name_to_mask(tok);
        script_keys[script_count].delay_ms = delay > 0 ? delay : SCRIPT_STEP_MS;
        script_count++;
    }
    /* First key fires after one step, so the startup fetch has a moment to
     * land before the script starts pressing things. */
    if (script_count > 0) script_next_at = now_sec() + SCRIPT_STEP_MS / 1000.0;
}

static int script_poll(void)
{
    if (script_count == 0) return 0;
    double t = now_sec();
    if (t < script_next_at) return 0;

    if (script_pos >= script_count) {
        if (!script_hold) g_running = 0;
        return 0;
    }
    ScriptKey *k = &script_keys[script_pos++];
    script_next_at = t + k->delay_ms / 1000.0;
    return k->mask;
}

/* Translates one event through a device's MiSTer map.
 *
 * Axis bindings need the edge computed here rather than read off the event:
 * an analog trigger streams a run of intermediate values on the way down, so
 * "value crossed the threshold this time when it hadn't last time" is what
 * makes one pull into one press. Buttons are the plain value==1 edge. */
static int mapped_event_bits(int i, const struct input_event *ev)
{
    int bits = 0;
    for (int v = 0; v < PADV_COUNT; v++) {
        const PadBind *b = &input_map[i].v[v];

        if (b->kind == PADBIND_KEY) {
            if (ev->type == EV_KEY && ev->value == 1 && ev->code == b->code)
                bits |= padv_to_inp[v];
        } else if (b->kind == PADBIND_AXIS) {
            if (ev->type != EV_ABS || ev->code != b->code) continue;
            AxisState *st = &input_axis[i][v];
            int now = padmap_axis_pressed(ev->value, st->min, st->max, b->at_max);
            if (now && !st->pressed) bits |= padv_to_inp[v];
            st->pressed = now;
        }
    }
    return bits;
}

int input_poll(void)
{
    struct input_event ev;
    int mask = stdin_poll() | script_poll();
    for (int i = 0; i < input_count; i++) {
        /* Reading a removed device fails with ENODEV; that's how a
         * disconnect is noticed, since evdev has no other notification.
         * Dropping the slot here is what lets the same pad be picked up
         * again by the next rescan. */
        ssize_t got;
        while ((got = read(input_fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            /* Still drained, just not acted on — see input_mapped_any. Left
             * unread the queue would only fill up and deliver these later. */
            if (input_is_virtual[i] && input_mapped_any) continue;

            if (input_has_map[i]) {
                int bits = mapped_event_bits(i, &ev);
                if (input_debug && bits)
                    fprintf(stderr, "[input] %s %s code=%d val=%d -> %s (mapped)\n",
                            input_names[i], ev.type == EV_ABS ? "EV_ABS" : "EV_KEY",
                            ev.code, ev.value, inp_bit_name(bits & -bits));
                mask |= bits;
                continue;
            }

            /* Press edges only. Releases don't need tracking here: auto-repeat
             * reads the device's real current state instead of reconstructing
             * it from edges (see input_repeat). The kernel's own key repeat
             * (value 2) is ignored too — it only fires for real keyboards,
             * never for a gamepad button or D-pad hat, and runs at whatever
             * rate the console is configured for. */
            if (ev.type == EV_KEY && ev.value == 1) {
                int code = ev.code;
                if (input_swap_ab[i]) {
                    if      (code == BTN_SOUTH) code = BTN_EAST;
                    else if (code == BTN_EAST)  code = BTN_SOUTH;
                }
                /* Only reached for a device with no MiSTer map of its own —
                 * once one exists, input_mapped_any takes this whole device
                 * out of play (see input_poll's loop).
                 *
                 * This filter used to be justified by two claims that raw
                 * evdev capture on hardware (2026-09-09) disproved, so don't
                 * restore them:
                 *
                 *  - "MiSTer grabs wired USB pads exclusively, so the echo is
                 *    the only path." There is no EVIOCGRAB. A plain reader
                 *    gets both key and axis events off the pad's own node
                 *    while the MiSTer process holds it open — an fd in
                 *    /proc/PID/fd means the
                 *    device is OPEN, not grabbed. Both paths are live at once,
                 *    which is exactly why trusting both double-fires.
                 *  - "PageUp/PageDown aren't in MiSTer's OSD table and are an
                 *    arbitrary, colliding echo." They are deliberate: the echo
                 *    carries L as KEY_PAGEUP and R as KEY_PAGEDOWN, the same
                 *    pairing used below.
                 *
                 * What the echo genuinely does NOT carry is SELECT and START
                 * (MiSTer keeps that chord for its own OSD), which is why an
                 * echo-only input path can't work for this app and the pad's
                 * own node has to be primary. */
                if (input_is_virtual[i] &&
                    code != KEY_UP && code != KEY_DOWN &&
                    code != KEY_LEFT && code != KEY_RIGHT &&
                    code != KEY_ENTER && code != KEY_ESC && code != KEY_BACK) {
                    if (input_debug)
                        fprintf(stderr, "[input] %s EV_KEY code=%d val=%d -> DROPPED "
                                        "(not trusted from MiSTer virtual input)\n",
                                input_names[i], code, ev.value);
                    continue;
                }
                int bit = 0;
                switch (code) {
                case BTN_EAST:               bit = INP_A;      break;
                case BTN_SOUTH:              bit = INP_B;      break;
                /* Enter/Esc are the intuitive confirm/cancel pair; X/Z are
                 * the de facto SNES-emulator standard (RetroArch/SNES9x
                 * default keyboard mapping) matching the SNES pad's A
                 * (right) / B (bottom) positions — both work. */
                case KEY_ENTER:
                case KEY_X:                  bit = INP_A;      break;
                case KEY_ESC:
                case KEY_BACK:
                case KEY_BACKSPACE:
                case KEY_Z:                  bit = INP_B;      break;
                case BTN_START: case KEY_PAUSE: case KEY_HOME: bit = INP_START;  break;
                case BTN_SELECT: case KEY_TAB:  bit = INP_SELECT; break;
                case KEY_UP:                     bit = INP_UP;    break;
                case KEY_DOWN:                   bit = INP_DOWN;  break;
                case KEY_LEFT:                   bit = INP_LEFT;  break;
                case KEY_RIGHT:                  bit = INP_RIGHT; break;
                case BTN_TL: case KEY_PAGEUP:    bit = INP_L;     break;
                case BTN_TR: case KEY_PAGEDOWN:  bit = INP_R;     break;
                }
                if (input_debug)
                    fprintf(stderr, "[input] %s EV_KEY code=%d val=%d -> %s\n",
                            input_names[i], code, ev.value,
                            bit ? inp_bit_name(bit) : "unmapped");
                mask |= bit;
            } else if (ev.type == EV_ABS) {
                if (input_debug && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y))
                    fprintf(stderr, "[input] %s EV_ABS %s value=%d\n",
                            input_names[i],
                            ev.code == ABS_HAT0X ? "HAT0X" : "HAT0Y", ev.value);
                if (ev.code == ABS_HAT0Y) {
                    if (ev.value == -1) mask |= INP_UP;
                    if (ev.value ==  1) mask |= INP_DOWN;
                }
                if (ev.code == ABS_HAT0X) {
                    if (ev.value == -1) mask |= INP_LEFT;
                    if (ev.value ==  1) mask |= INP_RIGHT;
                }
            }
        }
        if (got < 0 && (errno == ENODEV || errno == EBADF)) {
            input_drop_slot(i);
            i--;            /* the slot now holds the next device */
        }
    }
    return mask;
}

/* ── navigation auto-repeat ──────────────────────────────────────────────────
 * Holding a direction should keep scrolling instead of demanding one press
 * per row — a library of any size was otherwise a genuine repetitive-strain
 * hazard to get through.
 *
 * Deliberately NOT folded into input_poll()'s return value: repeats are only
 * wanted where the action is "move a cursor". Applying them everywhere would
 * make a held UP skip through music tracks at ten a second on the now-playing
 * screen, and a held LEFT pile up an enormous accumulated video seek. Callers
 * that want repeat OR this in explicitly; everything else keeps seeing clean
 * press edges only.
 *
 * Two rates: a slower one to start with (so a deliberate single-row nudge
 * doesn't overshoot), then faster once it's clear the direction is being held
 * on purpose, which is what makes crossing a few hundred rows bearable. */
#define REPEAT_DELAY_SEC  0.35   /* hold this long before repeating at all */
#define REPEAT_SLOW_SEC   0.11
#define REPEAT_FAST_SEC   0.045
#define REPEAT_RAMP_AFTER 6      /* repeats at the slow rate before speeding up */

static int    repeat_mask  = 0;      /* direction currently repeating, 0 = none */
static double repeat_next  = 0.0;
static int    repeat_count = 0;
static int    repeat_suppressed = 0; /* see input_repeat_reset */

static void input_repeat_reset(void)
{
    repeat_mask  = 0;
    repeat_next  = 0.0;
    repeat_count = 0;
    /* Called from input_drain, i.e. right after a screen change. Whatever is
     * physically held at that moment shouldn't immediately start scrolling
     * the screen you just arrived at, so repeat stays parked until the user
     * lets go of everything. */
    repeat_suppressed = 1;
}

/* Asks each device what it is ACTUALLY holding right now, via EVIOCGKEY /
 * EVIOCGABS, rather than reconstructing it from the press/release stream.
 *
 * This is a correctness fix, not an optimisation. Reconstructing held state
 * from edges means a single missed release leaves a direction stuck "down"
 * forever — which showed up on hardware as auto-repeat that wouldn't stop
 * until the button was pressed again. There are several ways to miss one
 * here: input_drain() deliberately discards pending events at every screen
 * change, a hotplugged pad can be reopened under a new event node mid-press
 * leaving a stale entry nothing ever clears, and MiSTer's OSD echoes pad
 * input onto a second virtual device whose event pairing this code does not
 * control. Reading the state directly makes all of those unrepresentable:
 * there is no accumulated state to go wrong, and a device that has gone away
 * simply fails the ioctl and contributes nothing. */
/* Is this mapped device holding `verb` right now? Same
 * ask-the-device-directly principle as the legacy path below, extended to
 * cover axis bindings — which matters because on a pad whose D-pad is an axis
 * the legacy EVIOCGABS(ABS_HAT0*) probe reads a real, existing, permanently
 * zero hat and concludes nothing is held. */
static int mapped_verb_held(int i, int verb)
{
    const PadBind *b = &input_map[i].v[verb];

    if (b->kind == PADBIND_KEY) {
        unsigned long keys[INPUT_NLONGS(KEY_MAX + 1)];
        memset(keys, 0, sizeof(keys));
        if (ioctl(input_fds[i], EVIOCGKEY(sizeof(keys)), keys) < 0) return 0;
        return INPUT_TEST_BIT(keys, b->code) ? 1 : 0;
    }
    if (b->kind == PADBIND_AXIS) {
        struct input_absinfo abs;
        if (ioctl(input_fds[i], EVIOCGABS(b->code), &abs) < 0) return 0;
        return padmap_axis_pressed(abs.value, input_axis[i][verb].min,
                                   input_axis[i][verb].max, b->at_max);
    }
    return 0;
}

int input_select_start_held(void)
{
    int select_down = 0, start_down = 0;
    for (int i = 0; i < input_count; i++) {
        if (input_is_virtual[i] && input_mapped_any) continue;
        if (input_has_map[i]) {
            if (mapped_verb_held(i, PADV_SELECT)) select_down = 1;
            if (mapped_verb_held(i, PADV_START))  start_down  = 1;
            continue;
        }
        unsigned long keys[INPUT_NLONGS(KEY_MAX + 1)];
        memset(keys, 0, sizeof(keys));
        if (ioctl(input_fds[i], EVIOCGKEY(sizeof(keys)), keys) < 0) continue;
        if (INPUT_TEST_BIT(keys, BTN_SELECT) || INPUT_TEST_BIT(keys, KEY_TAB))
            select_down = 1;
        if (INPUT_TEST_BIT(keys, BTN_START) || INPUT_TEST_BIT(keys, KEY_PAUSE) ||
            INPUT_TEST_BIT(keys, KEY_HOME))
            start_down = 1;
    }
    if (input_debug) {
        static int last_sel = -1, last_start = -1;
        if (select_down != last_sel || start_down != last_start) {
            fprintf(stderr, "[input] select_start_held: select=%d start=%d\n",
                    select_down, start_down);
            last_sel = select_down;
            last_start = start_down;
        }
    }
    return select_down && start_down;
}

static int input_nav_held(void)
{
    int mask = 0;
    for (int i = 0; i < input_count; i++) {
        if (input_is_virtual[i] && input_mapped_any) continue;
        if (input_has_map[i]) {
            static const int nav[] = { PADV_UP, PADV_DOWN, PADV_LEFT, PADV_RIGHT };
            for (size_t k = 0; k < sizeof(nav) / sizeof(nav[0]); k++)
                if (mapped_verb_held(i, nav[k])) mask |= padv_to_inp[nav[k]];
            continue;
        }
        unsigned long keys[INPUT_NLONGS(KEY_MAX + 1)];
        memset(keys, 0, sizeof(keys));
        if (ioctl(input_fds[i], EVIOCGKEY(sizeof(keys)), keys) >= 0) {
            if (INPUT_TEST_BIT(keys, KEY_UP))    mask |= INP_UP;
            if (INPUT_TEST_BIT(keys, KEY_DOWN))  mask |= INP_DOWN;
            if (INPUT_TEST_BIT(keys, KEY_LEFT))  mask |= INP_LEFT;
            if (INPUT_TEST_BIT(keys, KEY_RIGHT)) mask |= INP_RIGHT;
        }
        /* Some D-pads arrive as a hat axis rather than as keys.
         *
         * The absent-axis case is safe (the ioctl fails, or reports 0, and
         * either means "nothing held"). What this CANNOT see is a pad that
         * declares ABS_HAT0X/Y and reports its D-pad somewhere else: the
         * 8BitDo M30 in X-input mode has both hat axes, pinned at 0, while
         * the D-pad drives ABS_X/ABS_Y — so this probe reads "nothing held"
         * throughout a real press and auto-repeat never starts. That pad is
         * handled by the mapped branch above; this is only the fallback for
         * a device MiSTer has no map for. */
        struct input_absinfo abs;
        if (ioctl(input_fds[i], EVIOCGABS(ABS_HAT0Y), &abs) >= 0) {
            if (abs.value < 0) mask |= INP_UP;
            if (abs.value > 0) mask |= INP_DOWN;
        }
        if (ioctl(input_fds[i], EVIOCGABS(ABS_HAT0X), &abs) >= 0) {
            if (abs.value < 0) mask |= INP_LEFT;
            if (abs.value > 0) mask |= INP_RIGHT;
        }
    }
    return mask;
}

/* Returns the nav bits that should act as though freshly pressed this tick
 * because they're being held down. Call once per main-loop iteration. */
int input_repeat(void)
{
    int held = input_nav_held();

    if (repeat_suppressed) {
        if (held) return 0;      /* still held over from before the screen change */
        repeat_suppressed = 0;   /* released — normal service resumes */
    }

    double t = now_sec();

    /* Any change in which direction is held restarts the delay — including
     * releasing one of two simultaneously held directions, which is the
     * right call: the surviving direction is then effectively a new press. */
    if (held != repeat_mask) {
        repeat_mask  = held;
        repeat_count = 0;
        repeat_next  = held ? t + REPEAT_DELAY_SEC : 0.0;
        return 0;
    }
    if (!held || t < repeat_next) return 0;

    repeat_next = t + (repeat_count >= REPEAT_RAMP_AFTER ? REPEAT_FAST_SEC : REPEAT_SLOW_SEC);
    repeat_count++;
    return held;
}

