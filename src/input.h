#ifndef INPUT_H
#define INPUT_H

/* All the ways a button press reaches the app — evdev gamepads/keyboards
 * on real hardware (including MiSTer's own virtual echo device), a raw-mode
 * stdin backend for desktop runs (MISTERFIN_STDIN=1), and scripted playback
 * for reproducible screenshots (MISTERFIN_KEYS). Every source funnels into
 * the same INP_* bitmask. */

#define INP_UP     0x01
#define INP_DOWN   0x02
#define INP_A      0x04
#define INP_B      0x08
#define INP_LEFT   0x10
#define INP_RIGHT  0x20
#define INP_START  0x40
#define INP_SELECT 0x80
#define INP_L      0x100
#define INP_R      0x200

/* Owned by main.c (set to 0 by its signal handlers to quit). The stdin
 * backend's 'q' key and a drained MISTERFIN_KEYS script also request quit
 * through it — that's the module's only reach into app lifecycle. */
extern volatile int g_running;

/* Scans /dev/input for new devices (and starts the desktop backends on
 * their first call). Safe to call repeatedly — only nodes not already
 * tracked get opened, which is what makes the main loop's periodic rescan
 * pick up a reconnected wireless pad. */
void input_open(void);

/* Non-blocking: drains every pending event and returns the union of fresh
 * press edges as INP_* bits (0 if nothing happened). Auto-repeat is NOT
 * folded in — see input_repeat. */
int input_poll(void);

/* Navigation auto-repeat: returns held direction bits at the repeat rate,
 * 0 otherwise. Kept separate from input_poll so only cursor-movement call
 * sites opt in — a held direction must not auto-repeat a seek or a track
 * skip. */
int input_repeat(void);

/* Discards whatever is queued and parks auto-repeat until every button is
 * released. Call on screen changes so a press meant for the old screen
 * can't act on the new one. */
void input_drain(void);

void input_close(void);

#endif
