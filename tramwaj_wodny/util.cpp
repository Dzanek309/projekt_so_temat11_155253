#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void die_perror(const char* what) {
    perror(what);                  // wypisuje: opis na stderr
    exit(1);                       // koñczy proces kodem 1
}

int64_t now_ms_monotonic(void) {
    struct timespec ts;                                                         // struktura na czas
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) die_perror("clock_gettime");  // czas monotoniczny: nie cofa siê przy zmianie zegara systemowego
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;                    // konwersja na milisekundy
}

int parse_i32(const char* s, int32_t* out) {
    if (!s || !*s) return -1;                       // odrzuæ NULL lub pusty string
    errno = 0;                                      // wyzeruj errno przed wywo³aniem strtol (¿eby wykryæ b³¹d)
    char* end = NULL;                               // wskaŸnik na pierwszy nieprzetworzony znak
    long v = strtol(s, &end, 10);                   // parsuj liczbê w systemie dziesiêtnym; strtol mo¿e ustawiæ errno
    if (errno != 0) return -1;                      // b³¹d zakresu/parsowania zg³oszony przez errno
    if (end == s || *end != '\0') return -1;        // brak cyfr (end==s) albo œmieci na koñcu (nie doszliœmy do '\0')
    if (v < INT32_MIN || v > INT32_MAX) return -1;  // sprawdzenie czy mieœci siê w int32_t (long mo¿e byæ szerszy)
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
    ts.tv_sec = ms / 1000;                  // pe³ne sekundy
    ts.tv_nsec = (ms % 1000) * 1000000L;    // reszta w nanosekundach
    while (nanosleep(&ts, &ts) != 0) {      // jeœli przerwane sygna³em, ts zostaje nadpisany pozosta³ym czasem
        if (errno == EINTR) continue;       // przerwane sygna³em: œpij dalej pozosta³y czas
        die_perror("nanosleep");            // inne b³êdy (np. EINVAL): zakoñcz program
    }
}
