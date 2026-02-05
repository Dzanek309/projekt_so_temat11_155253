#include "common.h"
#include "ipc.h"
#include "logging.h"
#include "util.h"

#include <cstdlib>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit = 0;      // flaga zakonczenia ustawiana w handlerze sygnalu (typ bezpieczny dla sygnalow)
static void on_term(int) { g_exit = 1; }      // handler SIGINT/SIGTERM: tylko ustawia flage (operacja async-signal-safe)

static void install_handlers(void) {
    struct sigaction sa;                      // struktura konfiguracji obslugi sygnalow
    memset(&sa, 0, sizeof(sa));               // wyzeruj dla pewnosci
    sa.sa_handler = on_term;                  // ustaw funkcje handlera
    sigemptyset(&sa.sa_mask);                 // brak dodatkowych sygnalow blokowanych w trakcie obslugi
    sa.sa_flags = 0;                          // brak specjalnych flag (np. SA_RESTART)

    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");   // podmien obsluge Ctrl+C
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)"); // podmien obsluge terminate
    if (sigaction(SIGHUP, &sa, NULL) != 0) die_perror("sigaction(SIGHUP)");
}

static void usage(void) {
    fprintf(stderr,                             // wypisz na stderr instrukcje uruchomienia
        "Usage (preferred / IPC):\n"
        "  dispatcher --shm <name> --sem-prefix <prefix> --msqid <id> --log <path>\n"
        "\n"
        "Usage (legacy):\n"
        "  dispatcher --captain-pid <pid> --log <path>\n"
        "  (then it will NOT observe END/shutdown from SHM)\n"
    );
}

static int sem_wait_nointr(sem_t* s) {
    while (sem_wait(s) != 0) {
        if (errno == EINTR) return -1;         // jesli przerwane sygnalem, ponow
        die_perror("sem_wait");               // inne bledy -> zakoncz
    }
    return 0;
}
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post"); // zwolnij semafor; blad -> zakoncz
}

static pid_t read_captain_pid_from_shm(ipc_handles_t* ipc) {
    if (sem_wait_nointr(ipc->sem_state) != 0) return 0;
    pid_t p = ipc->shm->captain_pid;          // odczytaj PID kapitana zapisany w pamieci wspoldzielonej
    sem_post_chk(ipc->sem_state);             // wyjdz z sekcji krytycznej
    return p;                                 // zwroc PID
}

static int should_exit_from_shm(ipc_handles_t* ipc) {
    if (sem_wait_nointr(ipc->sem_state) != 0) return 1;
    int shutdown = ipc->shm->shutdown;        // sprawdz flage globalnego shutdown
    int end_phase = (ipc->shm->phase == PHASE_END); // sprawdz czy kapitan jest w fazie koncowej
    sem_post_chk(ipc->sem_state);             // odblokuj
    return (shutdown || end_phase);           // wyjdz jesli ktorykolwiek warunek spelniony
}

int main(int argc, char** argv) {
    install_handlers();                       // zainstaluj handlery SIGINT/SIGTERM

    const char* shm_name = NULL;              // nazwa SHM dla trybu IPC
    const char* sem_prefix = NULL;            // prefiks semaforow dla trybu IPC
    const char* log_path = NULL;              // sciezka do pliku logow
    int msqid = -1;                           // id kolejki komunikatow System V
    pid_t captain_pid = -1;                   // PID procesu kapitana (cel sygnalow)

    for (int i = 1; i < argc; i++) {          // proste parsowanie argumentow CLI
        const char* a = argv[i];              // aktualny argument

        auto need_val = [&](const char* opt) -> const char* { // helper: pobierz wartosc opcji
            if (i + 1 >= argc) {              // brak wartosci po opcji
                fprintf(stderr, "Missing value for %s\n", opt);
                usage();
                _exit(2);                     // wyjscie natychmiastowe (bez atexit/flush stdio) z kodem bledu 2
            }
            return argv[++i];                 // przesun i i zwroc kolejny argument jako wartosc
        };

        if (strcmp(a, "--shm") == 0) {
            shm_name = need_val("--shm");     // ustaw nazwe SHM
        }
        else if (strcmp(a, "--sem-prefix") == 0) {
            sem_prefix = need_val("--sem-prefix"); // ustaw prefiks semaforow
        }
        else if (strcmp(a, "--msqid") == 0) {
            const char* v = need_val("--msqid");
            int32_t tmp;
            if (parse_i32(v, &tmp) != 0 || tmp < 0) {
                fprintf(stderr, "Invalid value for --msqid: %s (must be >= 0)\n", v);
                usage();
                return 2;
            }
            msqid = (int)tmp;
        }
        else if (strcmp(a, "--log") == 0) {
            log_path = need_val("--log");     // ustaw sciezke loga
        }
        else if (strcmp(a, "--captain-pid") == 0) {
            const char* v = need_val("--captain-pid");
            int32_t tmp;
            if (parse_i32(v, &tmp) != 0 || tmp <= 1) {
                fprintf(stderr, "Invalid value for --captain-pid: %s (must be > 1)\n", v);
                usage();
                return 2;
            }
            captain_pid = (pid_t)tmp;
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();                          // pokaz pomoc
            return 0;                         // i zakoncz sukcesem
        }
        else {
            fprintf(stderr, "Unknown arg: %s\n", a); // nieznana opcja
            usage();
            return 2;                         // blad uzycia
        }
    }

    if (!log_path) {                          // log jest wymagany w obu trybach
        fprintf(stderr, "dispatcher: missing --log\n");
        usage();
        return 2;
    }

    const int have_ipc = (shm_name && sem_prefix && msqid >= 0); // tryb IPC aktywny gdy sa kompletne parametry

    ipc_handles_t ipc;
    memset(&ipc, 0, sizeof(ipc));             // wyzeruj uchwyty IPC
    int ipc_opened = 0;                       // flaga: czy IPC zostalo poprawnie otwarte

    logger_t lg;
    memset(&lg, 0, sizeof(lg));               // wyzeruj strukture loggera
    lg.fd = -1;                               // jawnie ustaw brak deskryptora

    if (have_ipc) {
        if (ipc_open(&ipc, shm_name, sem_prefix, msqid) != 0) {  // podepnij sie do SHM/semaf./msq
            fprintf(stderr, "dispatcher: ipc_open failed\n");
            return 1;
        }
        ipc_opened = 1;                       // zaznacz aktywny IPC

        if (logger_open(&lg, log_path, ipc.sem_log) != 0) {      // otworz log z synchronizacja przez sem_log
            fprintf(stderr, "dispatcher: logger_open failed\n");
            ipc_close(&ipc);                  // posprzataj IPC przy bledzie
            return 1;
        }

        if (captain_pid < 0) {                // jesli PID nie podany na CLI
            captain_pid = read_captain_pid_from_shm(&ipc); // sprobuj odczytac z SHM
        }
    }
    else {
        fprintf(stderr, "dispatcher: running in LEGACY mode (no IPC)\n"); // tryb bez SHM/sem/msq
    }

    if (captain_pid <= 1) {                   // PID musi byc >1
        fprintf(stderr, "dispatcher: captain pid unknown/invalid (provide --captain-pid > 1 or IPC args)\n");
        if (ipc_opened) { logger_close(&lg); ipc_close(&ipc); }  // sprzatnij jesli cos otwarte
        return 2;
    }

    if (ipc_opened) {
        logf(&lg, "dispatcher", "started captain_pid=%d", (int)captain_pid); // log startu z PID kapitana
    }

    fprintf(stderr,                             // instrukcja sterowania z klawiatury
        "Dispatcher pid=%d. Commands:\n"
        "  1 + ENTER -> send SIGUSR1 (early depart)\n"
        "  2 + ENTER -> send SIGUSR2 (stop)\n",
        (int)getpid());

    while (!g_exit) {                          // petla glowna dopoki nie dostaniemy SIGINT/SIGTERM
        if (ipc_opened && should_exit_from_shm(&ipc)) { // jesli IPC: obserwuj END/shutdown zapisane w SHM
            logf(&lg, "dispatcher", "observed END/shutdown in SHM -> exit");
            break;                             // wyjdz z petli
        }

        fd_set rfds;
        FD_ZERO(&rfds);                        // wyczysc zestaw descriptorow do select
        FD_SET(STDIN_FILENO, &rfds);           // obserwuj stdin (komendy uzytkownika)

        struct timeval tv;
        tv.tv_sec = 0;                         // timeout 0.2s, aby okresowo sprawdzac SHM/g_exit
        tv.tv_usec = 200000;

        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv); // czekaj na wejscie lub timeout
        if (sel < 0) {
            if (errno == EINTR) continue;      // przerwane sygnalem -> wroc do petli i sprawdz g_exit
            perror("select");                  // inny blad select
            break;
        }
        if (sel == 0) continue;                // timeout -> iteracja (sprawdzenie SHM na gorze petli)

        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf)); // odczytaj wpisane znaki
        if (n <= 0) break;                     // EOF lub blad -> koniec

        char cmd = 0;
        for (ssize_t i = 0; i < n; i++) {      // znajdz pierwsza nie-biala litere jako komende
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') continue;
            cmd = buf[i];
            break;
        }

        if (cmd == '1') {                      // komenda: wczesny odjazd
            if (kill(captain_pid, SIGUSR1) != 0) { // wyslij SIGUSR1 do kapitana
                perror("kill(SIGUSR1)");       // blad wyslania (np. brak procesu / uprawnien)
                if (ipc_opened) logf(&lg, "dispatcher", "FAILED SIGUSR1 to captain=%d errno=%d", (int)captain_pid, errno); // log bledu
            }
            else {
                if (ipc_opened) logf(&lg, "dispatcher", "sent SIGUSR1 to captain=%d", (int)captain_pid); // log sukcesu
            }
        }
        else if (cmd == '2') {                 // komenda: stop
            if (kill(captain_pid, SIGUSR2) != 0) { // wyslij SIGUSR2 do kapitana
                perror("kill(SIGUSR2)");
                if (ipc_opened) logf(&lg, "dispatcher", "FAILED SIGUSR2 to captain=%d errno=%d", (int)captain_pid, errno);
            }
            else {
                if (ipc_opened) logf(&lg, "dispatcher", "sent SIGUSR2 to captain=%d", (int)captain_pid);
            }
        }
    }

    if (ipc_opened) {
        logf(&lg, "dispatcher", "EXIT (g_exit=%d)", (int)g_exit); // koncowy wpis w logu z powodem (czy przerwano sygnalem)
        logger_close(&lg);                      // zamknij logger
        ipc_close(&ipc);                        // odlacz sie od IPC
    }

    return 0;                                   // standardowe wyjscie
}
