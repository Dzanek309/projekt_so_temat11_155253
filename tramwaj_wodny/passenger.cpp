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

static void release_n(sem_t* s, int n) {
    for (int i = 0; i < n; i++) sem_post_chk(s);
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

    const pid_t me = getpid();
    const int desired_dir = a.desired_dir;           // -1 lub 0/1
    const int has_bike = (a.bike_flag == 1) ? 1 : 0; // 0/1 z launchera
    const int units = has_bike ? 2 : 1;

    logf(&lg, "passenger", "start desired_dir=%d bike=%d units=%d",
        desired_dir, has_bike, units);

    bool seat_reserved = false;
    bool bike_reserved = false;
    int  bridge_units_held = 0; // 0..units
    int  on_bridge = 0;
    int  boarded = 0;

    const int64_t kGiveUpMs = 15000; // 15s
    const int64_t start = now_ms_monotonic();

    while (!g_exit) {
        // snapshot stanu
        sem_wait_nointr(ipc.sem_state);
        shm_state_t snapshot = *ipc.shm;
        sem_post_chk(ipc.sem_state);

        if (snapshot.shutdown || snapshot.phase == PHASE_END) {
            logf(&lg, "passenger", "observed shutdown/END -> exit");
            break;
        }

        // timeout na próbê wejœcia (zanim wejdziemy na mostek)
        if (!boarded && (now_ms_monotonic() - start >= kGiveUpMs)) {
            logf(&lg, "passenger", "give up after %lldms without boarding",
                (long long)kGiveUpMs);
            break;
        }

        // czekaj na LOADING dla w³aœciwego kierunku
        if (snapshot.phase != PHASE_LOADING ||
            snapshot.boarding_open == 0 ||
            !desired_dir_ok(&snapshot, desired_dir)) {
            sleep_ms(20);
            continue;
        }

        // rezerwuj miejsce
        if (!seat_reserved) {
            if (sem_trywait_chk(ipc.sem_seats) != 0) {
                sleep_ms(10);
                continue;
            }
            seat_reserved = true;
            logf(&lg, "passenger", "reserved seat");
        }

        // rezerwuj rower (opcjonalnie)
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

        // rezerwuj mostek (units)
        if (bridge_units_held == 0) {
            int got = 0;
            for (int i = 0; i < units; i++) {
                if (sem_trywait_chk(ipc.sem_bridge) != 0) break;
                got++;
            }
            bridge_units_held = got;

            if (bridge_units_held != units) {
                // rollback mostek + rezerwacje
                release_n(ipc.sem_bridge, bridge_units_held);
                bridge_units_held = 0;

                if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
                if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

                sleep_ms(10);
                continue;
            }
        }

        // wejœcie na mostek: wymagamy dir NONE lub IN oraz nadal LOADING
        sem_wait_nointr(ipc.sem_state);

        if (ipc.shm->phase != PHASE_LOADING ||
            ipc.shm->boarding_open == 0 ||
            !desired_dir_ok(ipc.shm, desired_dir)) {
            sem_post_chk(ipc.sem_state);

            // rollback
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
            if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

            sleep_ms(10);
            continue;
        }

        if (!(ipc.shm->bridge.dir == BRIDGE_DIR_NONE || ipc.shm->bridge.dir == BRIDGE_DIR_IN)) {
            sem_post_chk(ipc.sem_state);

            // rollback
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
            if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

            sleep_ms(10);
            continue;
        }

        if (ipc.shm->bridge.dir == BRIDGE_DIR_NONE) ipc.shm->bridge.dir = BRIDGE_DIR_IN;

        bridge_node_t node;
        node.pid = me;
        node.units = (uint8_t)units;
        node.evicting = 0;

        if (bridge_push_back(ipc.shm, node) != 0) {
            sem_post_chk(ipc.sem_state);

            // rollback
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
            if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

            sleep_ms(10);
            continue;
        }

        on_bridge = 1;
        sem_post_chk(ipc.sem_state);

        logf(&lg, "passenger", "entered bridge (dir IN), waiting to board");

        // symulacja przejœcia
        sleep_ms(30);

        // czekaj a¿ bêdziesz z przodu i boarding wci¹¿ otwarty
        while (!g_exit) {
            sem_wait_nointr(ipc.sem_state);

            if (ipc.shm->phase != PHASE_LOADING || ipc.shm->boarding_open == 0) {
                sem_post_chk(ipc.sem_state);
                // w tej wersji: bez ewakuacji LIFO, po prostu wyjœcie (cleanup poni¿ej)
                logf(&lg, "passenger", "boarding closed while on bridge -> exit (stub)");
                goto finish;
            }

            bridge_node_t* fr = bridge_front(ipc.shm);
            if (fr && fr->pid == me) {
                bridge_node_t out;
                bridge_pop_front(ipc.shm, &out);
                if (ipc.shm->bridge.count == 0) ipc.shm->bridge.dir = BRIDGE_DIR_NONE;

                ipc.shm->onboard_passengers += 1;
                if (has_bike) ipc.shm->onboard_bikes += 1;

                int onboard = ipc.shm->onboard_passengers;
                int bikes = ipc.shm->onboard_bikes;

                sem_post_chk(ipc.sem_state);

                // zszed³em z mostka -> zwalniam mostek
                release_n(ipc.sem_bridge, bridge_units_held);
                bridge_units_held = 0;
                on_bridge = 0;

                boarded = 1;
                logf(&lg, "passenger", "BOARDED ship (onboard=%d bikes=%d)", onboard, bikes);
                break;
            }

            sem_post_chk(ipc.sem_state);
            sleep_ms(10);
        }

        break; // po próbie wejœcia na statek koñczymy
    }

finish:
    // cleanup zasobów (stub)
    if (bridge_units_held > 0) {
        release_n(ipc.sem_bridge, bridge_units_held);
        bridge_units_held = 0;
    }

    // jeœli staliœmy na mostku i nie weszliœmy na statek, nie usuwamy wpisu z kolejki (naprawimy w kolejnym commicie)
    (void)on_bridge;

    if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
    if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }

    logf(&lg, "passenger", "EXIT (boarded=%d exit_flag=%d)", boarded, (int)g_exit);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
