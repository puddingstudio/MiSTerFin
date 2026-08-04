#ifndef SESSION_H
#define SESSION_H

#include "jellyfin.h"

/* The server-push half of being a real Jellyfin session: a background
 * thread holding the /socket WebSocket open so the server can send this
 * client admin messages (DisplayMessage) and remote playback commands
 * (pause/play/stop from the dashboard's Active Sessions card). The thread
 * (re)registers capabilities on every connect and translates the wire
 * messages into SessionEvents; main's loop polls them with session_poll()
 * and decides what they mean for whatever screen is up — this module never
 * touches playback or the framebuffer itself.
 *
 * Plain-http servers only: the socket is a hand-rolled RFC6455 client over
 * a TCP socket (curl does all the https/TLS work elsewhere, but holding a
 * persistent websocket open is the one thing it can't do for us here). On
 * an https server the thread logs once and exits — everything else about
 * the app keeps working, there's just no push channel. */

typedef enum {
    SESSION_EV_MESSAGE,      /* DisplayMessage: header/text below */
    SESSION_EV_PLAY_PAUSE,   /* toggle */
    SESSION_EV_PAUSE,        /* pause only (no-op when already paused) */
    SESSION_EV_UNPAUSE,      /* resume only */
    SESSION_EV_STOP,
    SESSION_EV_NEXT,         /* next/previous track (music queue) */
    SESSION_EV_PREV
} SessionEventKind;

typedef struct {
    SessionEventKind kind;
    char header[96];         /* SESSION_EV_MESSAGE only; may be empty */
    char text[224];
} SessionEvent;

/* Starts the background thread. cfg must outlive the session (it's main's
 * global). Safe to call once; no-op if already running. The thread waits
 * for cfg->token to appear, so it can be started before Quick Connect
 * completes. */
void session_start(const JfConfig *cfg);

/* Pops one pending event. Returns 1 and fills *ev, or 0 if none. Call from
 * the main thread once per loop tick. */
int session_poll(SessionEvent *ev);

#endif
