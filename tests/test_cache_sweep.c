/* Cache path selection and cache_sweep_superseded(), which reclaims entries
 * whose filename key was superseded by a change of fetch width.
 *
 * This is the one piece of code in the app that deletes the user's files, so
 * what it must NOT delete matters more than what it must. Every assertion
 * below about a survivor is the real point of the test: the live cache, a
 * different extension, a subdirectory, and anything a user happened to leave
 * in the folder all have to come through untouched. */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

/* util.c pulls in stb_image for the two loaders next to the sweep; the
 * implementation lives in main.c, which cannot be linked here. The sweep
 * touches neither loader. */
unsigned char *stbi_load(char const *f, int *x, int *y, int *n, int req)
{
    (void)f; (void)x; (void)y; (void)n; (void)req;
    assert(!"cache_sweep_superseded must not decode images");
    return NULL;
}

static char DIR_PATH[] = "/tmp/misterfin_test_sweep";

static void put(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", DIR_PATH, name);
    FILE *f = fopen(p, "wb");
    assert(f);
    fputs("x", f);
    fclose(f);
}

static int there(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", DIR_PATH, name);
    return access(p, F_OK) == 0;
}

static int checks;
#define CHECK(c, m) do { checks++; if (!(c)) { \
    printf("  FAIL %s (line %d)\n", m, __LINE__); exit(1); } } while (0)

int main(void)
{
    char cache_path[256];
    unsetenv("MISTERFIN_CACHE_ROOT");
    CHECK(cache_dir_path("covercache", cache_path, sizeof(cache_path)),
          "default cache path builds");
    CHECK(!strcmp(cache_path, "/media/fat/misterfin/covercache"),
          "default remains the MiSTer install path");

    assert(system("rm -rf /tmp/misterfin_test_cache_root") == 0);
    setenv("MISTERFIN_CACHE_ROOT", "/tmp/misterfin_test_cache_root/", 1);
    CHECK(cache_dir_path("gridcache", cache_path, sizeof(cache_path)),
          "overridden cache path builds");
    CHECK(!strcmp(cache_path, "/tmp/misterfin_test_cache_root/gridcache"),
          "trailing slash is not duplicated");
    CHECK(cache_dir_ensure("gridcache", cache_path, sizeof(cache_path)),
          "override root and child are created");
    struct stat cache_st;
    CHECK(stat(cache_path, &cache_st) == 0 && S_ISDIR(cache_st.st_mode),
          "created cache child is a directory");
    CHECK(!cache_dir_path("../escape", cache_path, sizeof(cache_path)),
          "cache child cannot contain a slash");
    unsetenv("MISTERFIN_CACHE_ROOT");

    /* Fresh tree every run. */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/sub", DIR_PATH, DIR_PATH);
    assert(system(cmd) == 0);

    /* Superseded — these are the targets. */
    put("abc123_tag.img");
    put("def456_tag.img");
    put("hghi789_tag.img");           /* an old hero, 'h' prefix, still no _w */
    /* Current key — must survive. */
    put("abc123_tag_w180.img");
    put("hghi789_tag_w640.img");
    /* Not ours — must survive. */
    put("abc123_tag.dat");            /* right dir, wrong extension */
    put("notes.txt");
    put("README");                    /* no extension at all */
    put(".img");                      /* pathological: name IS the extension */

    int n = cache_sweep_superseded(DIR_PATH, ".img", "_w");

    CHECK(n == 3, "removed exactly the three superseded entries");
    CHECK(!there("abc123_tag.img"),      "superseded cover gone");
    CHECK(!there("def456_tag.img"),      "superseded cover gone");
    CHECK(!there("hghi789_tag.img"),     "superseded hero gone");

    CHECK(there("abc123_tag_w180.img"),  "live cover survives");
    CHECK(there("hghi789_tag_w640.img"), "live hero survives");
    CHECK(there("abc123_tag.dat"),       "other extension survives");
    CHECK(there("notes.txt"),            "unrelated file survives");
    CHECK(there("README"),               "extensionless file survives");
    CHECK(there(".img"),                 "a name equal to the extension survives");
    CHECK(there("sub"),                  "subdirectory survives");

    /* Run again: this is the property that makes it safe to call on every
     * launch — after the first pass there is nothing left that matches, so it
     * is a no-op rather than a second round of deletion. */
    int again = cache_sweep_superseded(DIR_PATH, ".img", "_w");
    CHECK(again == 0, "second pass removes nothing");
    CHECK(there("abc123_tag_w180.img"), "live cover still there after re-run");
    CHECK(there("hghi789_tag_w640.img"), "live hero still there after re-run");

    /* Degenerate arguments do nothing rather than something creative. */
    CHECK(cache_sweep_superseded(NULL, ".img", "_w") == 0, "NULL dir");
    CHECK(cache_sweep_superseded(DIR_PATH, NULL, "_w") == 0, "NULL ext");
    CHECK(cache_sweep_superseded(DIR_PATH, ".img", NULL) == 0, "NULL keep");
    CHECK(cache_sweep_superseded(DIR_PATH, "", "_w") == 0, "empty ext");
    CHECK(cache_sweep_superseded(DIR_PATH, ".img", "") == 0, "empty keep");
    CHECK(cache_sweep_superseded("/tmp/misterfin_no_such_dir", ".img", "_w") == 0,
          "missing directory");
    CHECK(there("abc123_tag_w180.img"), "nothing lost to the degenerate calls");

    snprintf(cmd, sizeof cmd, "rm -rf %s", DIR_PATH);
    (void)system(cmd);
    assert(system("rm -rf /tmp/misterfin_test_cache_root") == 0);
    printf("cache sweep: %d checks, 0 failures\n", checks);
    return 0;
}
