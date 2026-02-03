#include "cli.h"
#include "util.h"
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int streq(const char* a, const char* b) { return strcmp(a, b) == 0; } // pomocnicze porównanie stringów (równe -> 1)

static int need_arg(int i, int argc) { return (i + 1) < argc; } // sprawdza czy po opcji jest jeszcze wartoœæ

void cli_print_usage_tramwaj(void) {
    fprintf(stderr, // wypisuje instrukcjê uruchomienia programu g³ównego (launcher)
        "Usage:\n"
        "  tramwaj --N <int> --M <int> --K <int> --T1 <ms> --T2 <ms> --R <int> --P <int> [--bike-prob <0..1>] [--log <path>]\n"
        "Example:\n"
        "  ./tramwaj --N 20 --M 5 --K 6 --T1 1000 --T2 1500 --R 8 --P 60 --bike-prob 0.3 --log simulation.log\n");
}

void cli_print_usage_dispatcher(void) {
    fprintf(stderr, // wypisuje instrukcjê uruchomienia dispatcher’a
        "Usage:\n"
        "  dispatcher --captain-pid <pid> --log <path>\n"
        "  (IPC args are optional for dispatcher)\n");
}

void cli_print_usage_captain(void) {
    fprintf(stderr, // wypisuje instrukcjê uruchomienia kapitana (wymaga IPC)
        "Usage:\n"
        "  captain --shm <name> --sem-prefix <prefix> --msqid <id> --log <path>\n");
}

void cli_print_usage_passenger(void) {
    fprintf(stderr, // wypisuje instrukcjê uruchomienia pasa¿era (IPC + opcjonalne dir/bike)
        "Usage:\n"
        "  passenger --shm <name> --sem-prefix <prefix> --msqid <id> --log <path> [--dir 0|1] [--bike 0|1]\n"
        "  dir: 0 Krakow->Tyniec, 1 Tyniec->Krakow\n");
}

static void init_defaults(cli_args_t* a) {
    memset(a, 0, sizeof(*a));                                 // wyzeruj ca³¹ strukturê argumentów
    a->bike_prob = 0.0;                                       // domyœlnie brak rowerów (prawdopodobieñstwo)
    a->msqid = -1;                                            // -1 oznacza „nieustawione” dla id kolejki
    a->captain_pid = -1;                                      // -1 oznacza „nieustawione” dla PID kapitana
    a->desired_dir = -1;                                      // -1 oznacza „losowo/nieustawione” dla kierunku pasa¿era
    a->bike_flag = -1;                                        // -1 oznacza „losowo/nieustawione” dla flagi roweru
    a->interactive = 1;                                       // domyœlnie tryb interaktywny dispatchera w³¹czony
    snprintf(a->log_path, sizeof(a->log_path), "simulation.log"); // domyœlna œcie¿ka do logu
}

int cli_parse_launcher(int argc, char** argv, cli_args_t* out) {
    if (!out) return -1;                                      // brak wskaŸnika wyjœciowego -> b³¹d
    init_defaults(out);                                       // ustaw wartoœci domyœlne

    for (int i = 1; i < argc; i++) {                                // iteruj po argumentach (pomijaj¹c argv[0])
        const char* k = argv[i];                                // bie¿¹cy klucz/opcja
        if (streq(k, "--N") && need_arg(i, argc)) {               // liczba pasa¿erów
            if (parse_i32(argv[++i], &out->N) != 0) return -1;      // parsuj int32 i zapisz
        }
        else if (streq(k, "--M") && need_arg(i, argc)) {          // maks. liczba rowerów
            if (parse_i32(argv[++i], &out->M) != 0) return -1;
        }
        else if (streq(k, "--K") && need_arg(i, argc)) {          // pojemnoœæ tramwaju (miejsca/units)
            if (parse_i32(argv[++i], &out->K) != 0) return -1;
        }
        else if (streq(k, "--T1") && need_arg(i, argc)) {         // czas ³adowania/boarding (ms)
            if (parse_i32(argv[++i], &out->T1_ms) != 0) return -1;
        }
        else if (streq(k, "--T2") && need_arg(i, argc)) {         // czas rejsu/przejazdu (ms)
            if (parse_i32(argv[++i], &out->T2_ms) != 0) return -1;
        }
        else if (streq(k, "--R") && need_arg(i, argc)) {          // liczba kursów
            if (parse_i32(argv[++i], &out->R) != 0) return -1;
        }
        else if (streq(k, "--P") && need_arg(i, argc)) {          // maks. liczba procesów pasa¿erów/limit (wg common.h)
            if (parse_i32(argv[++i], &out->P) != 0) return -1;
        }
        else if (streq(k, "--bike-prob") && need_arg(i, argc)) {  // prawdopodobieñstwo, ¿e pasa¿er ma rower
            if (parse_double(argv[++i], &out->bike_prob) != 0) return -1;
        }
        else if (streq(k, "--log") && need_arg(i, argc)) {        // œcie¿ka loga
            snprintf(out->log_path, sizeof(out->log_path), "%s", argv[++i]);
        }
        else if (streq(k, "--help")) {                           // help -> wypisz usage i zakoñcz „specjalnym” kodem
            cli_print_usage_tramwaj();
            return 1;                                             // 1 oznacza „pokazano help”
        }
        else {                                                  // nieznany argument -> b³¹d parsowania
            fprintf(stderr, "Unknown arg: %s\n", k);
            return -1;
        }
    }
    return 0;                                                 // sukces
}

int cli_validate_launcher(const cli_args_t* a, char* err, int err_sz) {
    if (!a) return -1;                                        // brak wejœcia -> b³¹d
    if (a->N <= 0) { snprintf(err, err_sz, "N must be > 0"); return -1; }                  // N dodatnie
    if (a->M < 0 || a->M >= a->N) { snprintf(err, err_sz, "M must be >=0 and M < N"); return -1; } // M w [0, N)
    if (a->K <= 0 || a->K >= a->N) { snprintf(err, err_sz, "K must be >0 and K < N"); return -1; } // K w (0, N)
    if (a->K > MAX_K) { snprintf(err, err_sz, "K too large (max %d)", MAX_K); return -1; }         // K nie przekracza MAX_K
    if (a->T1_ms <= 0 || a->T2_ms <= 0) { snprintf(err, err_sz, "T1 and T2 must be > 0 (ms)"); return -1; } // czasy dodatnie
    if (a->R <= 0) { snprintf(err, err_sz, "R must be > 0"); return -1; }                          // liczba kursów dodatnia
    if (a->P < 0 || a->P > MAX_P) { snprintf(err, err_sz, "P must be in [0..%d]", MAX_P); return -1; }      // P w dozwolonym zakresie
    if (a->bike_prob < 0.0 || a->bike_prob > 1.0) { snprintf(err, err_sz, "bike-prob must be in [0..1]"); return -1; } // prawdopodobieñstwo 0..1
    if (!a->log_path[0]) { snprintf(err, err_sz, "log path empty"); return -1; }                   // œcie¿ka niepusta
    return 0;                                                // walidacja OK
}

int cli_parse_child_common(int argc, char** argv, cli_args_t* out) {
    if (!out) return -1;                                      // brak wyjœcia -> b³¹d
    init_defaults(out);                                       // ustaw domyœlne wartoœci
    // wymagane: --shm --sem-prefix --msqid --log
    for (int i = 1; i < argc; i++) {                                // przejdŸ po argumentach i zbierz wspólne parametry IPC
        const char* k = argv[i];
        if (streq(k, "--shm") && need_arg(i, argc)) {             // nazwa SHM
            snprintf(out->shm_name, sizeof(out->shm_name), "%s", argv[++i]);
        }
        else if (streq(k, "--sem-prefix") && need_arg(i, argc)) { // prefiks semaforów
            snprintf(out->sem_prefix, sizeof(out->sem_prefix), "%s", argv[++i]);
        }
        else if (streq(k, "--msqid") && need_arg(i, argc)) {      // id kolejki msq
            if (parse_i32(argv[++i], (int32_t*)&out->msqid) != 0) return -1; // parsuj do int32 i zapisz do msqid (rzutowanie wskaŸnika)
        }
        else if (streq(k, "--log") && need_arg(i, argc)) {        // œcie¿ka loga
            snprintf(out->log_path, sizeof(out->log_path), "%s", argv[++i]);
        }
        else if (streq(k, "--help")) {                           // help obs³u¿y parser roli
            return 1;
        }
        else { /* ignore unknown here; handled by role parser */ } // nieznane opcje zostan¹ sprawdzone w parserze konkretnej roli
    }
    if (!out->shm_name[0] || !out->sem_prefix[0] || out->msqid < 0) return -1; // brak wymaganych IPC -> b³¹d
    return 0;                                                                 // wspólne argumenty poprawne
}

int cli_parse_dispatcher(int argc, char** argv, cli_args_t* out) {
    if (!out) return -1;                                      // brak wyjœcia -> b³¹d
    init_defaults(out);                                       // domyœlne wartoœci
    for (int i = 1; i < argc; i++) {                                // parsuj argumenty dispatchera
        const char* k = argv[i];
        if (streq(k, "--captain-pid") && need_arg(i, argc)) {      // wymagany PID kapitana w tym parserze
            int32_t tmp;
            if (parse_i32(argv[++i], &tmp) != 0) return -1;          // waliduj jako int32
            out->captain_pid = (pid_t)tmp;                         // zapisz jako pid_t
        }
        else if (streq(k, "--log") && need_arg(i, argc)) {         // œcie¿ka loga
            snprintf(out->log_path, sizeof(out->log_path), "%s", argv[++i]);
        }
        else if (streq(k, "--non-interactive")) {                 // opcja: brak czytania z stdin
            out->interactive = 0;
        }
        else if (streq(k, "--help")) {                            // help -> sygna³ do wypisania usage
            return 1;
        }
        else {                                                   // nieznany argument -> b³¹d
            fprintf(stderr, "Unknown arg: %s\n", k);
            return -1;
        }
    }
    if (out->captain_pid <= 1) return -1;                      // PID 0/1/ujemny -> nieprawid³owy
    return 0;                                                  // sukces
}

int cli_parse_passenger(int argc, char** argv, cli_args_t* out) {
    if (!out) return -1;                                      // brak wyjœcia -> b³¹d
    int r = cli_parse_child_common(argc, argv, out);          // najpierw parsuj wspólne IPC (shm/sem/msqid/log)
    if (r != 0) return r;                                     // jeœli b³¹d lub help -> propaguj
    // dodatkowe
    for (int i = 1; i < argc; i++) {                                // parsuj opcje specyficzne dla pasa¿era
        const char* k = argv[i];
        if (streq(k, "--dir") && need_arg(i, argc)) {             // kierunek (0/1)
            int32_t d;
            if (parse_i32(argv[++i], &d) != 0) return -1;
            out->desired_dir = d;                                 // zapis
        }
        else if (streq(k, "--bike") && need_arg(i, argc)) {        // flaga roweru (0/1)
            int32_t b;
            if (parse_i32(argv[++i], &b) != 0) return -1;
            out->bike_flag = b;
        }
    }

    // --dir: dozwolone {0,1} albo -1 (losowo/nieustawione)
    if (!(out->desired_dir == -1 || out->desired_dir == 0 || out->desired_dir == 1)) {
        fprintf(stderr, "Invalid --dir: %d (allowed: 0, 1)\n", (int)out->desired_dir);
        return -1;
    }
    // --bike: dozwolone {0,1} albo -1 (losowo/nieustawione)
    if (!(out->bike_flag == -1 || out->bike_flag == 0 || out->bike_flag == 1)) {
        fprintf(stderr, "Invalid --bike: %d (allowed: 0, 1)\n", (int)out->bike_flag);
        return -1;
    }

    return 0;
}
