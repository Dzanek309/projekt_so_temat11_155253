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

static volatile sig_atomic_t g_exit = 0;      // flaga zako�czenia ustawiana w handlerze sygna�u (typ bezpieczny dla sygna��w)
static void on_term(int) { g_exit = 1; }      // handler SIGINT/SIGTERM: tylko ustawia flag� (operacja async-signal-safe)

static void install_handlers(void) {
    struct sigaction sa;                      // struktura konfiguracji obs�ugi sygna��w
    memset(&sa, 0, sizeof(sa));               // wyzeruj dla pewno�ci
    sa.sa_handler = on_term;                  // ustaw funkcj� handlera
    sigemptyset(&sa.sa_mask);                 // brak dodatkowych sygna��w blokowanych w trakcie obs�ugi
    sa.sa_flags = 0;                          // brak specjalnych flag (np. SA_RESTART)

    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");   // podmie� obs�ug� Ctrl+C
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)"); // podmie� obs�ug� terminate
    if (sigaction(SIGHUP, &sa, NULL) != 0) die_perror("sigaction(SIGHUP)");
}

static void usage(void) {
    fprintf(stderr,                             // wypisz na stderr instrukcj� uruchomienia
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
        if (errno == EINTR) return -1;         // je�li przerwane sygna�em, pon�w
        die_perror("sem_wait");               // inne b��dy -> zako�cz
    }
    return 0;
}
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post"); // zwolnij semafor; b��d -> zako�cz
}

static pid_t read_captain_pid_from_shm(ipc_handles_t* ipc) {
    if (sem_wait_nointr(ipc->sem_state) != 0) return 0;
    pid_t p = ipc->shm->captain_pid;          // odczytaj PID kapitana zapisany w pami�ci wsp�dzielonej
    sem_post_chk(ipc->sem_state);             // wyjd� z sekcji krytycznej
    return p;                                 // zwr�� PID
}

static int should_exit_from_shm(ipc_handles_t* ipc) {
    if (sem_wait_nointr(ipc->sem_state) != 0) return 1;
    int shutdown = ipc->shm->shutdown;        // sprawd� flag� globalnego shutdown
    int end_phase = (ipc->shm->phase == PHASE_END); // sprawd� czy kapitan jest w fazie ko�cowej
    sem_post_chk(ipc->sem_state);             // odblokuj
    return (shutdown || end_phase);           // wyjd� je�li kt�rykolwiek warunek spe�niony
}

int main(int argc, char** argv) {
    install_handlers();                       // zainstaluj handlery SIGINT/SIGTERM

    const char* shm_name = NULL;              // nazwa SHM dla trybu IPC
    const char* sem_prefix = NULL;            // prefiks semafor�w dla trybu IPC
    const char* log_path = NULL;              // �cie�ka do pliku log�w
    int msqid = -1;                           // id kolejki komunikat�w System V
    pid_t captain_pid = -1;                   // PID procesu kapitana (cel sygna��w)

    for (int i = 1; i < argc; i++) {          // proste parsowanie argument�w CLI
        const char* a = argv[i];              // aktualny argument

        auto need_val = [&](const char* opt) -> const char* { // helper: pobierz warto�� opcji
            if (i + 1 >= argc) {              // brak warto�ci po opcji
                fprintf(stderr, "Missing value for %s\n", opt);
                usage();
                _exit(2);                     // wyj�cie natychmiastowe (bez atexit/flush stdio) z kodem b��du 2
            }
            return argv[++i];                 // przesu� i i zwr�� kolejny argument jako warto��
            };

        if (strcmp(a, "--shm") == 0) {
            shm_name = need_val("--shm");     // ustaw nazw� SHM
        }
        else if (strcmp(a, "--sem-prefix") == 0) {
            sem_prefix = need_val("--sem-prefix"); // ustaw prefiks semafor�w
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
            log_path = need_val("--log");     // ustaw �cie�k� loga
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
            usage();                          // poka� pomoc
            return 0;                         // i zako�cz sukcesem
        }
        else {
            fprintf(stderr, "Unknown arg: %s\n", a); // nieznana opcja
            usage();
            return 2;                         // b��d u�ycia
        }
    }

    if (!log_path) {                          // log jest wymagany w obu trybach
        fprintf(stderr, "dispatcher: missing --log\n");
        usage();
        return 2;
    }

    const int have_ipc = (shm_name && sem_prefix && msqid >= 0); // tryb IPC aktywny gdy s� kompletne parametry

    ipc_handles_t ipc;
    memset(&ipc, 0, sizeof(ipc));             // wyzeruj uchwyty IPC
    int ipc_opened = 0;                       // flaga: czy IPC zosta�o poprawnie otwarte

    logger_t lg;
    memset(&lg, 0, sizeof(lg));               // wyzeruj struktur� loggera
    lg.fd = -1;                               // jawnie ustaw brak deskryptora

    if (have_ipc) {
        if (ipc_open(&ipc, shm_name, sem_prefix, msqid) != 0) {  // podepnij si� do SHM/semaf./msq
            fprintf(stderr, "dispatcher: ipc_open failed\n");
            return 1;
        }
        ipc_opened = 1;                       // zaznacz aktywny IPC

        if (logger_open(&lg, log_path, ipc.sem_log) != 0) {      // otw�rz log z synchronizacj� przez sem_log
            fprintf(stderr, "dispatcher: logger_open failed\n");
            ipc_close(&ipc);                  // posprz�taj IPC przy b��dzie
            return 1;
        }

        if (captain_pid < 0) {                // je�li PID nie podany na CLI
            captain_pid = read_captain_pid_from_shm(&ipc); // spr�buj odczyta� z SHM
        }
    }
    else {
        fprintf(stderr, "dispatcher: running in LEGACY mode (no IPC)\n"); // tryb bez SHM/sem/msq
    }

    if (captain_pid <= 1) {                   // PID musi by� >1
        fprintf(stderr, "dispatcher: captain pid unknown/invalid (provide --captain-pid > 1 or IPC args)\n");
        if (ipc_opened) { logger_close(&lg); ipc_close(&ipc); }  // sprz�tnij je�li co� otwarte
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

    while (!g_exit) {                          // p�tla g��wna dop�ki nie dostaniemy SIGINT/SIGTERM
        if (ipc_opened && should_exit_from_shm(&ipc)) { // je�li IPC: obserwuj END/shutdown zapisane w SHM
            logf(&lg, "dispatcher", "observed END/shutdown in SHM -> exit");
            break;                             // wyjd� z p�tli
        }

        fd_set rfds;
        FD_ZERO(&rfds);                        // wyczy�� zestaw descriptor�w do select
        FD_SET(STDIN_FILENO, &rfds);           // obserwuj stdin (komendy u�ytkownika)

        struct timeval tv;
        tv.tv_sec = 0;                         // timeout 0.2s, aby okresowo sprawdza� SHM/g_exit
        tv.tv_usec = 200000;

        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv); // czekaj na wej�cie lub timeout
        if (sel < 0) {
            if (errno == EINTR) continue;      // przerwane sygna�em -> wr�� do p�tli i sprawd� g_exit
            perror("select");                  // inny b��d select
            break;
        }
        if (sel == 0) continue;                // timeout -> iteracja (sprawdzenie SHM na g�rze p�tli)

        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf)); // odczytaj wpisane znaki
        if (n <= 0) break;                     // EOF lub b��d -> koniec

        char cmd = 0;
        for (ssize_t i = 0; i < n; i++) {      // znajd� pierwsz� nie-bia�� liter� jako komend�
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') continue;
            cmd = buf[i];
            break;
        }

        if (cmd == '1') {                      // komenda: wczesny odjazd
            if (kill(captain_pid, SIGUSR1) != 0) { // wy�lij SIGUSR1 do kapitana
                perror("kill(SIGUSR1)");       // b��d wys�ania (np. brak procesu / uprawnie�)
                if (ipc_opened) logf(&lg, "dispatcher", "FAILED SIGUSR1 to captain=%d errno=%d", (int)captain_pid, errno); // log b��du
            }
            else {
                if (ipc_opened) logf(&lg, "dispatcher", "sent SIGUSR1 to captain=%d", (int)captain_pid); // log sukcesu
            }
        }
        else if (cmd == '2') {                 // komenda: stop
            if (kill(captain_pid, SIGUSR2) != 0) { // wy�lij SIGUSR2 do kapitana
                perror("kill(SIGUSR2)");
                if (ipc_opened) logf(&lg, "dispatcher", "FAILED SIGUSR2 to captain=%d errno=%d", (int)captain_pid, errno);
            }
            else {
                if (ipc_opened) logf(&lg, "dispatcher", "sent SIGUSR2 to captain=%d", (int)captain_pid);
            }
        }
    }

    if (ipc_opened) {
        logf(&lg, "dispatcher", "EXIT (g_exit=%d)", (int)g_exit); // ko�cowy wpis w logu z powodem (czy przerwano sygna�em)
        logger_close(&lg);                      // zamknij logger
        ipc_close(&ipc);                        // od��cz si� od IPC
    }

    return 0;                                   // standardowe wyj�cie
}
