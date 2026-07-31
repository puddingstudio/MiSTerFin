#ifndef UPDATE_H
#define UPDATE_H

/* GitHub release check + one-button in-app update (the About screen's
 * footer). All the work happens on detached background threads; the UI
 * never blocks on it and just polls update_get_state() every frame it
 * draws the footer. */

typedef enum { UPD_CHECKING, UPD_OK, UPD_AVAILABLE, UPD_FAILED } UpdateState;
typedef enum { INST_IDLE, INST_DOWNLOADING, INST_DONE, INST_FAILED } InstallState;

/* Kicks off the release check against GitHub in a detached thread. Call
 * once at startup; the About screen shows whatever state it reaches.
 * No-op-safe if the network is down — the check just fails and the About
 * screen shows no update line. */
void update_check_start(void);

/* Starts downloading/installing the release found by the check, if one is
 * available and no install is already running. Safe to call repeatedly
 * (e.g. on every A press while the About screen is up) — only the first
 * call starts anything. */
void update_start_install(void);

/* Snapshot of both state machines for drawing, taken under the module's
 * own locks. latest (may be NULL) receives the release tag the check
 * found, e.g. "v0.9.7" — only meaningful when *upd is UPD_AVAILABLE. */
void update_get_state(UpdateState *upd, InstallState *inst,
                      char *latest, int latest_len);

#endif
