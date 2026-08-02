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
