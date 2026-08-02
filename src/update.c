/* GitHub release check + in-app update — same pattern as MiSTerDVD.
 * Extracted verbatim from main.c; see update.h for the interface. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "update.h"
#include "json.h"
#include "jellyfin.h"   /* jf_log_line only — see its own comment */

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static UpdateState     g_upd_state = UPD_CHECKING;
static char            g_upd_latest[32] = {0};
static pthread_mutex_t g_upd_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Checks github.com/puddingstudio/MiSTerFin's latest release tag against
 * APP_VERSION. No-op-safe if that repo doesn't exist yet (this project is
 * local-only for now) — the request just fails and the About screen shows
 * no update line, same as any other network hiccup. */
/* Runs a command with an explicit argv and NO SHELL, optionally capturing
 * stdout. Same reasoning as jf_curl_run in jellyfin.c: the update path
 * interpolates a GitHub-supplied release tag, and with a shell that tag is
 * parsed by /bin/sh. Returns 1 if the command exited 0. */
static int run_no_shell(char *const argv[], char *out, int outlen)
{
    if (out && outlen > 0) out[0] = '\0';

    int pfd[2];
    if (out && pipe(pfd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(pfd[0]); close(pfd[1]); }
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (out) { dup2(pfd[1], STDOUT_FILENO); close(pfd[0]); close(pfd[1]); }
        else if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out) {
        close(pfd[1]);
        int len = 0;
        for (;;) {
            ssize_t got = read(pfd[0], out + len, (size_t)(outlen - 1 - len));
            if (got <= 0) break;
            len += (int)got;
            if (len >= outlen - 1) break;
        }
        out[len] = '\0';
        close(pfd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A release tag is pasted into a download URL and a filename, so it gets a
 * strict allowlist rather than being trusted. Real tags look like "v0.9.3".
 * Returns 1 if the tag is safe to use. */
static int update_tag_is_sane(const char *tag)
{
    if (!tag || !tag[0] || strlen(tag) > 24) return 0;
    for (const char *p = tag; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_';
        if (!ok) return 0;
    }
    return 1;
}

/* Deliberately WITHOUT -k, unlike requests to the user's own Jellyfin server.
 * A self-signed certificate is normal for a home media server, so verification
 * is relaxed there; github.com has a valid certificate and there is no reason
 * to accept anything else. It matters more here than anywhere else in the app:
 * this path decides which binary gets written over misterfin-arm and run at
 * next launch, so accepting a substituted response is accepting a substituted
 * executable. No amount of quoting downstream fixes that. */
/* curl on the stock MiSTer image has no working default CA path — verifying
 * github fails with "unable to get local issuer certificate" even though a
 * perfectly good CA bundle ships in the image, curl just doesn't look there.
 * Point --cacert at the first bundle that exists so TLS verification stays ON
 * (the whole reason the updater dropped -k) instead of silently failing every
 * update check on real hardware. Returns NULL only if none is found. */
static const char *ca_bundle_path(void)
{
    static const char *const cands[] = {
        "/etc/ssl/certs/cacert.pem",
        "/etc/ssl/cert.pem",
        "/usr/lib/python3.9/site-packages/certifi/cacert.pem",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && st.st_size > 0) return cands[i];
    }
    return NULL;
}

static void *update_check_thread(void *arg)
{
    (void)arg;

    const char *ca = ca_bundle_path();
    char *argv[16];
    int n = 0;
    argv[n++] = (char *)"curl";     argv[n++] = (char *)"-fsSL";
    argv[n++] = (char *)"--proto";  argv[n++] = (char *)"=https";
    argv[n++] = (char *)"--tlsv1.2";
    if (ca) { argv[n++] = (char *)"--cacert"; argv[n++] = (char *)ca; }
    argv[n++] = (char *)"--max-time"; argv[n++] = (char *)"8";
    argv[n++] = (char *)"https://api.github.com/repos/puddingstudio/MiSTerFin/releases/latest";
    argv[n] = NULL;

    char buf[8192];
    if (!run_no_shell(argv, buf, sizeof(buf))) goto fail;

    /* Parsed properly rather than scanned for — the same reason every other
     * response in this app is. */
    JsonDoc doc;
    if (!json_parse(&doc, buf)) goto fail;
    char tag[32] = {0};
    json_copy_str(&doc, NULL, "tag_name", tag, sizeof(tag));
    json_free(&doc);

    if (!update_tag_is_sane(tag)) goto fail;

    pthread_mutex_lock(&g_upd_mutex);
    strncpy(g_upd_latest, tag, sizeof(g_upd_latest) - 1);
    g_upd_state = (strcmp(tag, APP_VERSION) == 0) ? UPD_OK : UPD_AVAILABLE;
    pthread_mutex_unlock(&g_upd_mutex);
    jf_log_line("update check: running=%s latest=%s -> %s", APP_VERSION, tag,
                g_upd_state == UPD_OK ? "up to date" : "update available");
    return NULL;

fail:
    pthread_mutex_lock(&g_upd_mutex);
    g_upd_state = UPD_FAILED;
    pthread_mutex_unlock(&g_upd_mutex);
    jf_log_line("update check: running=%s -> failed (network or GitHub unreachable)", APP_VERSION);
    return NULL;
}

static InstallState    g_inst_state = INST_IDLE;
static pthread_mutex_t g_inst_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_inst(InstallState s)
{
    pthread_mutex_lock(&g_inst_mutex);
    g_inst_state = s;
    pthread_mutex_unlock(&g_inst_mutex);
}

/* Downloads the tagged release zip, copies assets in place immediately
 * (safe while running), and writes an apply-update script for
 * MiSTerFin.sh to run on the NEXT launch — the running binary/mplayer-arm
 * can't safely overwrite themselves (ETXTBSY) while still executing. */
static void *install_thread(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&g_upd_mutex);
    char tag[32];
    strncpy(tag, g_upd_latest, sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = '\0';
    pthread_mutex_unlock(&g_upd_mutex);

    /* Re-checked here, not just where it was parsed: the tag crosses a mutex
     * and a thread boundary in between, and this is the point where it turns
     * into a URL. */
    if (!update_tag_is_sane(tag)) { set_inst(INST_FAILED); return NULL; }

    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/puddingstudio/MiSTerFin/releases/download/%s/misterfin-%s.zip",
             tag, tag);

    {
        const char *ca = ca_bundle_path();
        char *argv[16];
        int n = 0;
        argv[n++] = (char *)"curl";     argv[n++] = (char *)"-fsSL";
        argv[n++] = (char *)"--proto";  argv[n++] = (char *)"=https";
        argv[n++] = (char *)"--tlsv1.2";
        if (ca) { argv[n++] = (char *)"--cacert"; argv[n++] = (char *)ca; }
        argv[n++] = (char *)"--max-time"; argv[n++] = (char *)"120";
        argv[n++] = url; argv[n++] = (char *)"-o"; argv[n++] = (char *)"/tmp/misterfin-update.zip";
        argv[n] = NULL;
        if (!run_no_shell(argv, NULL, 0)) { set_inst(INST_FAILED); return NULL; }
    }

    {
        char *const argv[] = { (char *)"rm", (char *)"-rf",
                               (char *)"/tmp/misterfin-update/", NULL };
        run_no_shell(argv, NULL, 0);
    }
    {
        /* -qq quiet, -o overwrite. Info-ZIP refuses absolute and ../ paths by
         * default, so a crafted archive can't escape the destination. */
        char *const argv[] = { (char *)"unzip", (char *)"-qq", (char *)"-o",
                               (char *)"/tmp/misterfin-update.zip",
                               (char *)"-d", (char *)"/tmp/misterfin-update/", NULL };
        if (!run_no_shell(argv, NULL, 0)) { set_inst(INST_FAILED); return NULL; }
    }

    static const char *const asset_copies[][3] = {
        { "-r", "/tmp/misterfin-update/misterfin/font/.",    "/media/fat/misterfin/font/"    },
        { "-r", "/tmp/misterfin-update/misterfin/subfont/.", "/media/fat/misterfin/subfont/" },
        { "-r", "/tmp/misterfin-update/misterfin/toasty/.",  "/media/fat/misterfin/toasty/"  },
        { NULL, "/tmp/misterfin-update/misterfin/about.png", "/media/fat/misterfin/about.png" },
    };
    for (size_t i = 0; i < sizeof(asset_copies) / sizeof(asset_copies[0]); i++) {
        char *argv[6];
        int n = 0;
        argv[n++] = (char *)"cp";
        if (asset_copies[i][0]) argv[n++] = (char *)asset_copies[i][0];
        argv[n++] = (char *)asset_copies[i][1];
        argv[n++] = (char *)asset_copies[i][2];
        argv[n]   = NULL;
        run_no_shell(argv, NULL, 0);
    }

    FILE *f = fopen("/tmp/misterfin_apply_update.sh", "w");
    if (f) {
        fprintf(f, "#!/bin/bash\n");
        fprintf(f, "cp /tmp/misterfin-update/misterfin/misterfin-arm /media/fat/misterfin/misterfin-arm\n");
        fprintf(f, "cp /tmp/misterfin-update/misterfin/mplayer-arm   /media/fat/misterfin/mplayer-arm\n");
        fprintf(f, "chmod +x /media/fat/misterfin/misterfin-arm /media/fat/misterfin/mplayer-arm\n");
        fprintf(f, "rm -rf /tmp/misterfin-update/ /tmp/misterfin-update.zip\n");
        fclose(f);
        chmod("/tmp/misterfin_apply_update.sh", 0755);
    }

    set_inst(INST_DONE);
    return NULL;
}

void update_check_start(void)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, update_check_thread, NULL);
    pthread_attr_destroy(&attr);
}

void update_start_install(void)
{
    pthread_mutex_lock(&g_inst_mutex);
    if (g_inst_state != INST_IDLE) { pthread_mutex_unlock(&g_inst_mutex); return; }
    g_inst_state = INST_DOWNLOADING;
    pthread_mutex_unlock(&g_inst_mutex);

    pthread_t tid;
    pthread_create(&tid, NULL, install_thread, NULL);
    pthread_detach(tid);
}

void update_get_state(UpdateState *upd, InstallState *inst,
                      char *latest, int latest_len)
{
    pthread_mutex_lock(&g_upd_mutex);
    if (upd) *upd = g_upd_state;
    if (latest && latest_len > 0) {
        strncpy(latest, g_upd_latest, (size_t)latest_len - 1);
        latest[latest_len - 1] = '\0';
    }
    pthread_mutex_unlock(&g_upd_mutex);

    pthread_mutex_lock(&g_inst_mutex);
    if (inst) *inst = g_inst_state;
    pthread_mutex_unlock(&g_inst_mutex);
}
