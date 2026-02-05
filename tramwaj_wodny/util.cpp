#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void die_perror(const char* what) {
    perror(what);                  // wypisuje: opis na stderr
    exit(1);                       // konczy proces kodem 1
}

int64_t now_ms_monotonic(void) {
    struct timespec ts;                                                         // struktura na czas
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) die_perror("clock_gettime");  // czas monotoniczny: nie cofa sie przy zmianie zegara systemowego
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;                    // konwersja na milisekundy
}

int parse_i32(const char* s, int32_t* out) {
    if (!s || !*s) return -1;                       // odrzuc NULL lub pusty string
    errno = 0;                                      // wyzeruj errno przed wywolaniem strtol (zeby wykryc blad)
    char* end = NULL;                               // wskaznik na pierwszy nieprzetworzony znak
    long v = strtol(s, &end, 10);                   // parsuj liczbe w systemie dziesietnym; strtol moze ustawic errno
    if (errno != 0) return -1;                      // blad zakresu/parsowania zgloszony przez errno
    if (end == s || *end != '\0') return -1;        // brak cyfr (end==s) albo smieci na koncu (nie doszlismy do '\0')
    if (v < INT32_MIN || v > INT32_MAX) return -1;  // sprawdzenie czy miesci sie w int32_t (long moze byc szerszy)
    *out = (int32_t)v;                              // zapis wyniku po walidacji zakresu
    return 0;                                       // sukces
}

int parse_double(const char* s, double* out) {
    if (!s || !*s) return -1;
    errno = 0;
    char* end = NULL;
    double v = strtod(s, &end);
    if (errno != 0) return -1;
    if (end == s || *end != '\0') return -1;        // brak cyfr lub dodatkowe znaki po liczbie
    *out = v;                                       // zapis wyniku
    return 0;                                       // sukces
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;                  // pelne sekundy
    ts.tv_nsec = (ms % 1000) * 1000000L;    // reszta w nanosekundach
    while (nanosleep(&ts, &ts) != 0) {      // jesli przerwane sygnalem, ts zostaje nadpisany pozostalyм czasem
        if (errno == EINTR) continue;       // przerwane sygnalem: spij dalej pozostaly czas
        die_perror("nanosleep");            // inne bledy (np. EINVAL): zakoncz program
    }
}
