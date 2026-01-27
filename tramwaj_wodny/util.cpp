#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void die_perror(const char* what) {
    perror(what);
    exit(1);
}

int64_t now_ms_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) die_perror("clock_gettime");
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int parse_i32(const char* s, int32_t* out) {
    if (!s || !*s) return -1;
    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0) return -1;
    if (end == s || *end != '\0') return -1;
    if (v < INT32_MIN || v > INT32_MAX) return -1;
    *out = (int32_t)v;
    return 0;
}

int parse_double(const char* s, double* out) {
    if (!s || !*s) return -1;
    errno = 0;
    char* end = NULL;
    double v = strtod(s, &end);
    if (errno != 0) return -1;
    if (end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}
