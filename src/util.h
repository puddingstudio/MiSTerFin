#ifndef UTIL_H
#define UTIL_H

/* Monotonic seconds — the one timebase everything animates and times
 * against (immune to wall-clock jumps, unlike time()). */
double now_sec(void);

#endif
