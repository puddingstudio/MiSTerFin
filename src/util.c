#include <time.h>
#include "util.h"

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#include <unistd.h>
#include "stb_image.h"   /* declarations only — the implementation is compiled in main.c */

uint8_t *load_image_tmp(const char *tmp_path, int *w, int *h)
{
    int channels = 0;
    uint8_t *px = stbi_load(tmp_path, w, h, &channels, 4);
    unlink(tmp_path);
    return px;
}

uint8_t *load_image_keep(const char *path, int *w, int *h)
{
    int channels = 0;
    return stbi_load(path, w, h, &channels, 4);
}

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEFAULT_CACHE_ROOT "/media/fat/misterfin"

static const char *cache_root(void)
{
    const char *root = getenv("MISTERFIN_CACHE_ROOT");
    return (root && root[0]) ? root : DEFAULT_CACHE_ROOT;
}

int cache_dir_path(const char *name, char *out, size_t outsz)
{
    if (!name || !name[0] || strchr(name, '/') || !out || outsz == 0) return 0;
    const char *root = cache_root();
    size_t len = strlen(root);
    int n = snprintf(out, outsz, "%s%s%s", root,
                     (len > 0 && root[len - 1] == '/') ? "" : "/", name);
    return n >= 0 && (size_t)n < outsz;
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0) return 1;
    if (errno != EEXIST) return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int cache_dir_ensure(const char *name, char *out, size_t outsz)
{
    const char *root = cache_root();
    if (!ensure_dir(root) || !cache_dir_path(name, out, outsz)) return 0;
    return ensure_dir(out);
}

int cache_sweep_superseded(const char *dir, const char *ext, const char *keep_substr)
{
    if (!dir || !ext || !keep_substr || !*ext || !*keep_substr) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;                      /* no cache yet — nothing to do */

    size_t extlen = strlen(ext);
    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len <= extlen) continue;                       /* also skips "." / ".." */
        if (strcmp(n + len - extlen, ext) != 0) continue;  /* wrong extension */
        if (strstr(n, keep_substr)) continue;              /* current key format */

        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", dir, n) >= (int)sizeof(path))
            continue;                      /* wouldn't be a name we wrote */

        /* stat rather than d_type: exFAT reports DT_UNKNOWN, and unlinking a
         * directory would merely fail rather than be caught. Only paid on the
         * pass that finds candidates — later runs match nothing. */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (unlink(path) == 0) removed++;
    }
    closedir(d);
    return removed;
}
