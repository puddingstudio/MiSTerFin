/* Jellyfin session push channel — see session.h for what and why.
 *
 * Wire protocol notes, all confirmed against a real Jellyfin 10.11 server:
 * the endpoint is GET <server>/socket?api_key=<token>&deviceId=<id> with a
 * standard RFC6455 upgrade. The server sends unmasked text frames of JSON
 * shaped {"MessageType": "...", "Data": ...}; the two message types this
 * client acts on are
 *   GeneralCommand { Name:"DisplayMessage", Arguments:{Header,Text,...} }
 *   Playstate      { Command:"PlayPause"|"Pause"|"Unpause"|"Stop"|
 *                    "NextTrack"|"PreviousTrack"|... }
 * and everything else (ForceKeepAlive, Sessions, SyncPlay chatter, ...) is
 * ignored. The server expects a {"MessageType":"KeepAlive"} text frame at
 * least as often as the ForceKeepAlive timeout it announces (default 60s);
 * a fixed 20s cadence satisfies every version without parsing the value. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

#include "session.h"
#include "json.h"

#define WS_MSG_MAX      32768   /* biggest JSON message kept; larger ones skipped */
#define KEEPALIVE_SECS  20
#define RECONNECT_SECS  10
#define EVENT_QUEUE_MAX 8

static const JfConfig *g_scfg;
static pthread_t       g_sthread;
static int             g_sstarted;

static SessionEvent    g_equeue[EVENT_QUEUE_MAX];
static int             g_ehead, g_ecount;
static pthread_mutex_t g_emutex = PTHREAD_MUTEX_INITIALIZER;

static void event_push(const SessionEvent *ev)
{
    pthread_mutex_lock(&g_emutex);
    if (g_ecount < EVENT_QUEUE_MAX) {
        g_equeue[(g_ehead + g_ecount) % EVENT_QUEUE_MAX] = *ev;
        g_ecount++;
    }
    /* Queue full = main thread stalled for many events' worth of time;
     * dropping the newest is fine for what these are. */
    pthread_mutex_unlock(&g_emutex);
}

int session_poll(SessionEvent *ev)
{
    int got = 0;
    pthread_mutex_lock(&g_emutex);
    if (g_ecount > 0) {
        *ev = g_equeue[g_ehead];
        g_ehead = (g_ehead + 1) % EVENT_QUEUE_MAX;
        g_ecount--;
        got = 1;
    }
    pthread_mutex_unlock(&g_emutex);
    return got;
}

/* ── low-level socket plumbing ───────────────────────────────────────────── */

/* Connect with a bounded timeout — a plain blocking connect() to a dead
 * address can hang for minutes, which would make reconnect cycles stack up. */
static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc != 0) {
        fd_set wf;
        FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv = { 10, 0 };
        if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) { close(fd); return -1; }
        int err = 0; socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd); return -1;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);

    /* Short receive timeout: the read loop uses it as its tick, so the
     * keepalive sender runs even when the server is silent. */
    struct timeval rto = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
    return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        p += n; len -= (size_t)n;
    }
    return 1;
}

/* Buffered reader so handshake leftovers (frame bytes that arrived in the
 * same packet as the HTTP 101 response) aren't lost. */
typedef struct {
    int     fd;
    uint8_t buf[4096];
    int     len, pos;
    double *keepalive_due;   /* wall-clock time the next KeepAlive is owed */
} WsReader;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int ws_send_frame(int fd, uint8_t opcode, const void *payload, size_t len);

/* Returns the next byte, blocking as needed; -1 on connection loss. Sends
 * the periodic KeepAlive whenever a receive timeout gives it the chance. */
static int reader_byte(WsReader *r)
{
    while (r->pos >= r->len) {
        ssize_t n = recv(r->fd, r->buf, sizeof(r->buf), 0);
        if (n > 0) { r->len = (int)n; r->pos = 0; break; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (mono_now() >= *r->keepalive_due) {
                const char ka[] = "{\"MessageType\":\"KeepAlive\"}";
                if (!ws_send_frame(r->fd, 0x1, ka, sizeof(ka) - 1)) return -1;
                *r->keepalive_due = mono_now() + KEEPALIVE_SECS;
            }
            continue;
        }
        return -1;   /* orderly close (0) or a real error */
    }
    return r->buf[r->pos++];
}

static int reader_exact(WsReader *r, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int b = reader_byte(r);
        if (b < 0) return 0;
        out[i] = (uint8_t)b;
    }
    return 1;
}

/* ── RFC6455 frames ──────────────────────────────────────────────────────── */

/* Client-to-server frames must be masked (the server drops the connection
 * on unmasked ones). The mask key doesn't need to be unpredictable — it
 * exists to defeat proxy cache poisoning, not for secrecy. */
static int ws_send_frame(int fd, uint8_t opcode, const void *payload, size_t len)
{
    uint8_t hdr[14];
    size_t  hn = 0;
    hdr[hn++] = 0x80 | opcode;   /* FIN + opcode */
    if (len < 126) {
        hdr[hn++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[hn++] = 0x80 | 126;
        hdr[hn++] = (uint8_t)(len >> 8);
        hdr[hn++] = (uint8_t)len;
    } else {
        return 0;   /* nothing this client sends is anywhere near 64KB */
    }
    uint8_t mask[4];
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)payload;
    for (int i = 0; i < 4; i++) { seed = seed * 1103515245 + 12345; mask[i] = (uint8_t)(seed >> 16); }
    memcpy(hdr + hn, mask, 4); hn += 4;

    if (!send_all(fd, hdr, hn)) return 0;
    if (len == 0) return 1;

    uint8_t chunk[512];
    const uint8_t *src = payload;
    size_t off = 0;
    while (off < len) {
        size_t c = len - off; if (c > sizeof(chunk)) c = sizeof(chunk);
        for (size_t i = 0; i < c; i++) chunk[i] = src[off + i] ^ mask[(off + i) & 3];
        if (!send_all(fd, chunk, c)) return 0;
        off += c;
    }
    return 1;
}

/* Reads one complete (possibly fragmented) text message into msg
 * (NUL-terminated). Returns 1 on message, 0 on connection loss. Control
 * frames are handled inline; oversized messages are drained and skipped. */
static int ws_read_message(WsReader *r, char *msg, size_t msg_cap)
{
    size_t have = 0;
    int    in_text = 0, skipping = 0;

    for (;;) {
        int b0 = reader_byte(r); if (b0 < 0) return 0;
        int b1 = reader_byte(r); if (b1 < 0) return 0;
        int fin    = b0 & 0x80;
        int opcode = b0 & 0x0F;
        int masked = b1 & 0x80;
        uint64_t plen = (uint64_t)(b1 & 0x7F);
        if (plen == 126) {
            uint8_t ext[2];
            if (!reader_exact(r, ext, 2)) return 0;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (!reader_exact(r, ext, 8)) return 0;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }
        uint8_t mask[4] = {0};
        if (masked && !reader_exact(r, mask, 4)) return 0;

        if (opcode == 0x8) {                     /* close */
            ws_send_frame(r->fd, 0x8, NULL, 0);
            return 0;
        }
        if (opcode == 0x9 || opcode == 0xA) {    /* ping / pong */
            uint8_t pbuf[125];
            if (plen > sizeof(pbuf)) return 0;   /* control frames are <=125 by spec */
            if (!reader_exact(r, pbuf, (size_t)plen)) return 0;
            if (masked) for (uint64_t i = 0; i < plen; i++) pbuf[i] ^= mask[i & 3];
            if (opcode == 0x9 && !ws_send_frame(r->fd, 0xA, pbuf, (size_t)plen)) return 0;
            continue;
        }

        int data_frame = (opcode == 0x1) || (opcode == 0x0 && in_text);
        if (opcode == 0x1) { in_text = 1; have = 0; skipping = 0; }
        if (!data_frame || plen > WS_MSG_MAX || have + plen >= msg_cap) skipping = 1;

        for (uint64_t i = 0; i < plen; i++) {
            int b = reader_byte(r);
            if (b < 0) return 0;
            if (!skipping) msg[have++] = (char)(masked ? b ^ mask[i & 3] : b);
        }

        if (fin) {
            if (data_frame && !skipping) { msg[have] = '\0'; return 1; }
            in_text = 0; have = 0; skipping = 0;   /* binary or skipped — wait for the next one */
        }
    }
}

/* ── handshake ───────────────────────────────────────────────────────────── */

static void base64_16(const uint8_t *in, char *out)   /* 16 bytes -> 24 chars + NUL */
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 15; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];  out[o++] = tbl[v & 63];
    }
    unsigned v = (unsigned)in[15] << 16;   /* 1 trailing byte -> "XX==" */
    out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '=';
    out[o] = '\0';
}

/* Splits cfg->server into host/port/path-prefix. Returns 0 for anything but
 * plain http (see session.h). */
static int parse_server(const char *server, char *host, size_t hostlen,
                         char *port, size_t portlen, char *prefix, size_t prefixlen)
{
    if (strncmp(server, "http://", 7) != 0) return 0;
    const char *h = server + 7;
    const char *slash = strchr(h, '/');
    const char *hend  = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t)(hend - h));

    const char *he = colon ? colon : hend;
    if ((size_t)(he - h) >= hostlen) return 0;
    memcpy(host, h, (size_t)(he - h)); host[he - h] = '\0';

    if (colon) snprintf(port, portlen, "%.*s", (int)(hend - colon - 1), colon + 1);
    else       snprintf(port, portlen, "80");

    snprintf(prefix, prefixlen, "%s", slash ? slash : "");
    return host[0] != '\0' && port[0] != '\0';
}

static int ws_connect(const JfConfig *cfg)
{
    char host[128], port[16], prefix[128];
    if (!parse_server(cfg->server, host, sizeof(host), port, sizeof(port),
                       prefix, sizeof(prefix)))
        return -1;

    int fd = tcp_connect(host, port);
    if (fd < 0) return -1;

    uint8_t rnd[16];
    int ur = open("/dev/urandom", O_RDONLY);
    if (ur >= 0) { if (read(ur, rnd, sizeof(rnd)) != sizeof(rnd)) memset(rnd, 0x5A, sizeof(rnd)); close(ur); }
    else memset(rnd, 0x5A, sizeof(rnd));
    char key[28];
    base64_16(rnd, key);

    /* The Authorization header is what ties this socket to the same session
     * the ordinary API requests build up — see jf_auth_header's comment. */
    char auth[320];
    jf_auth_header(cfg, auth, sizeof(auth));

    char req[1152];
    int rn = snprintf(req, sizeof(req),
        "GET %s/socket?api_key=%s&deviceId=%s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s\r\n"
        "\r\n",
        prefix, cfg->token, cfg->device_id, host, port, key, auth);
    if (rn <= 0 || rn >= (int)sizeof(req) || !send_all(fd, req, (size_t)rn)) {
        close(fd); return -1;
    }

    /* Read the response headers byte-by-byte so nothing past \r\n\r\n is
     * consumed — the first ws frame may already be in the socket. */
    char   resp[1024];
    size_t rl = 0;
    while (rl < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + rl, 1, 0);
        if (n <= 0) { close(fd); return -1; }
        rl++;
        if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp[rl] = '\0';
    if (!strstr(resp, " 101 ")) { close(fd); return -1; }
    return fd;
}

/* ── message dispatch ────────────────────────────────────────────────────── */

static void handle_message(char *msg)
{
    JsonDoc doc;
    if (!json_parse(&doc, msg)) return;

    char type[32];
    json_copy_str(&doc, NULL, "MessageType", type, sizeof(type));

    if (!strcmp(type, "GeneralCommand")) {
        char name[32];
        json_copy_str(&doc, NULL, "Data.Name", name, sizeof(name));
        if (!strcmp(name, "DisplayMessage")) {
            SessionEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = SESSION_EV_MESSAGE;
            json_copy_str(&doc, NULL, "Data.Arguments.Header", ev.header, sizeof(ev.header));
            json_copy_str(&doc, NULL, "Data.Arguments.Text",   ev.text,   sizeof(ev.text));
            if (ev.text[0] || ev.header[0]) event_push(&ev);
        }
    } else if (!strcmp(type, "Playstate")) {
        char cmd[24];
        json_copy_str(&doc, NULL, "Data.Command", cmd, sizeof(cmd));
        SessionEvent ev;
        memset(&ev, 0, sizeof(ev));
        if      (!strcmp(cmd, "PlayPause"))     ev.kind = SESSION_EV_PLAY_PAUSE;
        else if (!strcmp(cmd, "Pause"))         ev.kind = SESSION_EV_PAUSE;
        else if (!strcmp(cmd, "Unpause"))       ev.kind = SESSION_EV_UNPAUSE;
        else if (!strcmp(cmd, "Stop"))          ev.kind = SESSION_EV_STOP;
        else if (!strcmp(cmd, "NextTrack"))     ev.kind = SESSION_EV_NEXT;
        else if (!strcmp(cmd, "PreviousTrack")) ev.kind = SESSION_EV_PREV;
        else { json_free(&doc); return; }   /* Seek & friends: not supported */
        event_push(&ev);
    }
    json_free(&doc);
}

/* ── thread ──────────────────────────────────────────────────────────────── */

static void *session_thread(void *arg)
{
    (void)arg;

    /* Quick Connect may still be running — the token appears when it's done. */
    while (!g_scfg->token[0]) sleep(2);

    if (strncmp(g_scfg->server, "http://", 7) != 0) {
        jf_log_line("session: https server - no command socket (messages/remote control off)");
        return NULL;
    }

    int logged_up = 0;
    for (;;) {
        int fd = ws_connect(g_scfg);
        if (fd < 0) {
            sleep(RECONNECT_SECS);
            continue;
        }
        if (!logged_up) { jf_log_line("session: command socket connected"); logged_up = 1; }

        /* Every (re)connect: the server may have forgotten this device's
         * session while we were away. */
        jf_report_capabilities(g_scfg);

        double keepalive_due = mono_now() + KEEPALIVE_SECS;
        WsReader reader;
        memset(&reader, 0, sizeof(reader));
        reader.fd = fd;
        reader.keepalive_due = &keepalive_due;

        static char msg[WS_MSG_MAX + 1];
        while (ws_read_message(&reader, msg, sizeof(msg)))
            handle_message(msg);

        close(fd);
        jf_log_line("session: command socket lost, reconnecting");
        sleep(RECONNECT_SECS);
    }
    return NULL;
}

void session_start(const JfConfig *cfg)
{
    if (g_sstarted) return;
    g_scfg = cfg;
    if (pthread_create(&g_sthread, NULL, session_thread, NULL) == 0) {
        pthread_detach(g_sthread);
        g_sstarted = 1;
    }
}
