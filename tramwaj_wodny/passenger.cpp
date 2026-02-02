#include "common.h"
#include "ipc.h"
#include "cli.h"
#include "logging.h"
#include "util.h"

#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit = 0;
static void on_term(int) { g_exit = 1; }

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)");
}

static void sem_wait_nointr(sem_t* s) {
    while (sem_wait(s) != 0) {
        if (errno == EINTR) continue;
        die_perror("sem_wait");
    }
}
static int sem_trywait_chk(sem_t* s) {
    if (sem_trywait(s) != 0) return -1; // EAGAIN/EINTR -> "nie uda³o siê"
    return 0;
}
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post");
}

static int desired_dir_ok(const shm_state_t* s, int desired_dir) {
    if (desired_dir < 0) return 1;
    return (int)s->direction == desired_dir;
}

int main(int argc, char** argv) {
    cli_args_t a;
    int r = cli_parse_passenger(argc, argv, &a);
    if (r == 1) { cli_print_usage_passenger(); return 0; }
    if (r != 0) { cli_print_usage_passenger(); return 2; }

    install_handlers();

    ipc_handles_t ipc;
    if (ipc_open(&ipc, a.shm_name, a.sem_prefix, a.msqid) != 0) {
        fprintf(stderr, "passenger: ipc_open failed\n");
        return 1;
    }

    logger_t lg;
    if (logger_open(&lg, a.log_path, ipc.sem_log) != 0) {
        fprintf(stderr, "passenger: logger_open failed\n");
        ipc_close(&ipc);
        return 1;
    }

    const int desired_dir = a.desired_dir;           // -1 lub 0/1
    const int has_bike = (a.bike_flag == 1) ? 1 : 0; // w tej wersji: 0/1 z launchera

    logf(&lg, "passenger",
        "start desired_dir=%d bike=%d",
        desired_dir, has_bike);

    bool seat_reserved = false;
    bool bike_reserved = false;

    const int64_t kGiveUpMs = 15000; // 15s
    const int64_t start = now_ms_monotonic();

    while (!g_exit) {
        // szybki snapshot
        sem_wait_nointr(ipc.sem_state);
        shm_state_t snapshot = *ipc.shm;
        sem_post_chk(ipc.sem_state);

        if (snapshot.shutdown || snapshot.phase == PHASE_END) {
            logf(&lg, "passenger", "observed shutdown/END -> exit");
            break;
        }

        // timeout na próbê wejœcia (zanim cokolwiek zarezerwujemy)
        if (!seat_reserved && (now_ms_monotonic() - start >= kGiveUpMs)) {
            logf(&lg, "passenger", "give up after %lldms without reserving seat",
                (long long)kGiveUpMs);
            break;
        }

        // rezerwacje tylko w LOADING dla w³aœciwego kierunku
        if (snapshot.phase != PHASE_LOADING ||
            snapshot.boarding_open == 0 ||
            !desired_dir_ok(&snapshot, desired_dir)) {
            sleep_ms(50);
            continue;
        }

        // zarezerwuj miejsce
        if (!seat_reserved) {
            if (sem_trywait_chk(ipc.sem_seats) != 0) {
                sleep_ms(10);
                continue;
            }
            seat_reserved = true;
            logf(&lg, "passenger", "reserved seat");
        }

        // zarezerwuj rower
        if (has_bike && !bike_reserved) {
            if (sem_trywait_chk(ipc.sem_bikes) != 0) {
                // rollback seat
                sem_post_chk(ipc.sem_seats);
                seat_reserved = false;
                sleep_ms(10);
                continue;
            }
            bike_reserved = true;
            logf(&lg, "passenger", "reserved bike slot");
        }

        // na tym etapie: mamy rezerwacje, czekamy na dalsze fazy
        logf(&lg, "passenger", "waiting for trip end (stub without bridge/boarding)");
        while (!g_exit) {
            sem_wait_nointr(ipc.sem_state);
            int shutdown = ipc.shm->shutdown;
            phase_t ph = ipc.shm->phase;
            sem_post_chk(ipc.sem_state);

            if (shutdown || ph == PHASE_END) break;
            sleep_ms(50);
        }
        break;
    }

    // cleanup rezerwacji
    if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
    if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

    logf(&lg, "passenger", "EXIT (g_exit=%d)", (int)g_exit);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
