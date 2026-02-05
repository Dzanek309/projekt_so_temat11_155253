#include "common.h"
#include "ipc.h"
#include "cli.h"
#include "logging.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

static volatile sig_atomic_t g_early_depart = 0;
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_exit = 0;

static void on_sigusr1(int) { g_early_depart = 1; }
static void on_sigusr2(int) { g_stop = 1; }
static void on_term(int) { g_exit = 1; }

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) die_perror("sigaction(SIGUSR1)");

    sa.sa_handler = on_sigusr2;
    if (sigaction(SIGUSR2, &sa, NULL) != 0) die_perror("sigaction(SIGUSR2)");

    sa.sa_handler = on_term;
    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)");
    if (sigaction(SIGHUP, &sa, NULL) != 0) die_perror("sigaction(SIGHUP)");
}

static int sem_wait_nointr(sem_t* s) {
    while (sem_wait(s) != 0) {
        if (errno == EINTR) return -1;
        die_perror("sem_wait");
    }
    return 0;
}
static void sem_post_chk(sem_t* s) { if (sem_post(s) != 0) die_perror("sem_post"); }

static const char* dir_str(int d) {
    return (d == DIR_KRAKOW_TO_TYNIEC) ? "KRAKOW->TYNIEC" : "TYNIEC->KRAKOW";
}

// Kapitan wymusza zejscie od konca kolejki (LIFO) poprzez:
// - ustawienie phase=DEPARTING i boarding_open=0
// - ustawienie bridge.dir = OUT
// - petla: wybierz back, oznacz evicting, wyslij CMD_EVICT(pid), czekaj na ACK
// Dodatkowo: zliczamy ile osob zeszlo z mostka (ile evictow).
static int captain_clear_bridge(ipc_handles_t* ipc, logger_t* lg, int* out_left_bridge_people) {
    int left_cnt = 0;

    for (;;) {
        if (sem_wait_nointr(ipc->sem_state) != 0) return -1;

        if (ipc->shm->bridge.count == 0) {
            ipc->shm->bridge.dir = BRIDGE_DIR_NONE;
            sem_post_chk(ipc->sem_state);
            logf(lg, "captain", "bridge empty -> ok to depart");
            if (out_left_bridge_people) *out_left_bridge_people = left_cnt;
            return 0;
        }

        bridge_node_t* last = bridge_back(ipc->shm);
        if (!last) {
            sem_post_chk(ipc->sem_state);
            continue;
        }

        pid_t target = last->pid;
        last->evicting = 1;
        int trip = ipc->shm->trip_no;

        sem_post_chk(ipc->sem_state);

        // wyslij polecenie ewakuacji do konkretnego PID (mtype=PID)
        msg_cmd_t cmd;
        cmd.mtype = (long)target;
        cmd.cmd = CMD_EVICT;
        cmd.trip_no = trip;
        if (msgsnd(ipc->msqid, &cmd, sizeof(cmd) - sizeof(long), 0) != 0) {
            perror("msgsnd(CMD_EVICT)");
        }
        else {
            logf(lg, "captain", "evict request sent to pid=%d", (int)target);
        }

        // czekaj na ACK (mtype=1)
        msg_ack_t ack;
        for (;;) {
            ssize_t n = msgrcv(ipc->msqid, &ack, sizeof(ack) - sizeof(long), 1, 0);
            if (n < 0) {
                if (errno == EINTR && !g_exit) continue;
                if (errno == EINTR) return -1;
                perror("msgrcv(ACK)");
                break;
            }
            if (ack.pid == target) {
                left_cnt++;
                logf(lg, "captain", "ack from pid=%d (left_bridge=%d)", (int)target, left_cnt);
                break;
            }
            // Jesli przyjdzie inny ack, ignorujemy (w praktyce rzadkie przy sekwencyjnym evict).
        }
    }
}

static int set_phase(ipc_handles_t* ipc, logger_t* lg, phase_t ph, int boarding_open) {
    if (sem_wait_nointr(ipc->sem_state) != 0) return -1;
    ipc->shm->phase = ph;
    ipc->shm->boarding_open = boarding_open;
    sem_post_chk(ipc->sem_state);
    logf(lg, "captain", "phase=%d boarding_open=%d", (int)ph, boarding_open);
    return 0;
}

int main(int argc, char** argv) {
    cli_args_t a;
    int r = cli_parse_child_common(argc, argv, &a);
    if (r == 1) { cli_print_usage_captain(); return 0; }
    if (r != 0) { cli_print_usage_captain(); return 2; }

    install_handlers();

    ipc_handles_t ipc;
    if (ipc_open(&ipc, a.shm_name, a.sem_prefix, a.msqid) != 0) {
        fprintf(stderr, "captain: ipc_open failed\n");
        return 1;
    }

    logger_t lg;
    if (logger_open(&lg, a.log_path, ipc.sem_log) != 0) {
        fprintf(stderr, "captain: logger_open failed\n");
        ipc_close(&ipc);
        return 1;
    }

    logf(&lg, "captain", "started; shm=%s msqid=%d", a.shm_name, ipc.msqid);

    int trips_done = 0;

    while (!g_exit) {
        // Sprawdz shutdown z launchera
        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        int shutdown = ipc.shm->shutdown;
        sem_post_chk(ipc.sem_state);
        if (shutdown) {
            logf(&lg, "captain", "shutdown flag set -> END");
            if (set_phase(&ipc, &lg, PHASE_END, 0) != 0) break;
            break;
        }

        // reset jednorazowego sygnalu "early depart" na start tripu
        g_early_depart = 0;

        if (set_phase(&ipc, &lg, PHASE_LOADING, 1) != 0) break;

        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        ipc.shm->trip_no += 1;
        int my_trip = ipc.shm->trip_no;

        // snapshot kierunku dla statystyk tej podrozy
        int trip_dir = (int)ipc.shm->direction;

        ipc.shm->bridge.dir = BRIDGE_DIR_NONE;
        sem_post_chk(ipc.sem_state);

        // sleep(100);
        logf(&lg, "captain", "trip=%d direction=%d LOADING", my_trip, trip_dir);
        int64_t start = now_ms_monotonic();
        while (!g_exit) {
            // jesli sygnal2 dotarl w trakcie zaladunku: statek nie wyplywa, pasazerowie opuszczaja statek
            if (g_stop) {
                logf(&lg, "captain", "stop during LOADING -> cancel trip and UNLOADING");
                break;
            }
            if (g_early_depart) {
                logf(&lg, "captain", "early depart signal received");
                break;
            }
            int64_t now = now_ms_monotonic();
            if (now - start >= ipc.shm->T1_ms) {
                logf(&lg, "captain", "T1 elapsed -> depart");
                break;
            }
            sleep_ms(20);
        }

        // Zamknij boarding i przejda do DEPARTING
        if (set_phase(&ipc, &lg, PHASE_DEPARTING, 0) != 0) break;

        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
        sem_post_chk(ipc.sem_state);

        int trip_left_bridge = 0;
        if (captain_clear_bridge(&ipc, &lg, &trip_left_bridge) != 0) break;

        int trip_boarded_pax = 0;
        int trip_boarded_bikes = 0;
        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        trip_boarded_pax = ipc.shm->onboard_passengers;
        trip_boarded_bikes = ipc.shm->onboard_bikes;
        sem_post_chk(ipc.sem_state);

        if (g_stop) {
            if (set_phase(&ipc, &lg, PHASE_UNLOADING, 0) != 0) break;

            if (sem_wait_nointr(ipc.sem_state) != 0) break;
            ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
            sem_post_chk(ipc.sem_state);

            for (;;) {
                if (sem_wait_nointr(ipc.sem_state) != 0) break;
                int onboard = ipc.shm->onboard_passengers;
                sem_post_chk(ipc.sem_state);
                if (onboard == 0) break;
                sleep_ms(50);
            }
            if (g_exit) break;

            logf(&lg, "captain", "unloading complete (stop)");
            logf(&lg, "captain",
                "TRIP SUMMARY trip=%d route=%s passengers=%d bikes=%d left_bridge=%d",
                my_trip, dir_str(trip_dir), trip_boarded_pax, trip_boarded_bikes, trip_left_bridge);

            logf(&lg, "captain", "all passengers left after stop -> END");
            if (set_phase(&ipc, &lg, PHASE_END, 0) != 0) break;
            break;
        }

        logf(&lg, "captain", "sailing for T2=%dms", ipc.shm->T2_ms);
        if (set_phase(&ipc, &lg, PHASE_SAILING, 0) != 0) break;
        int64_t sail_start = now_ms_monotonic();
        while (!g_exit) {
            int64_t now = now_ms_monotonic();
            if (now - sail_start >= ipc.shm->T2_ms) break;
            sleep_ms(20);
        }

        logf(&lg, "captain", "arrived -> UNLOADING");
        if (set_phase(&ipc, &lg, PHASE_UNLOADING, 0) != 0) break;
        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
        sem_post_chk(ipc.sem_state);

        // czekaj az wszyscy zejda
        for (;;) {
            if (sem_wait_nointr(ipc.sem_state) != 0) break;
            int onboard = ipc.shm->onboard_passengers;
            sem_post_chk(ipc.sem_state);
            if (onboard == 0) break;
            sleep_ms(50);
        }
        if (g_exit) break;

        logf(&lg, "captain", "unloading complete");
        logf(&lg, "captain",
            "TRIP SUMMARY trip=%d route=%s passengers=%d bikes=%d left_bridge=%d",
            my_trip, dir_str(trip_dir), trip_boarded_pax, trip_boarded_bikes, trip_left_bridge);

        trips_done++;
        if (trips_done >= ipc.shm->R) {
            logf(&lg, "captain", "max trips R=%d reached -> END", ipc.shm->R);
            if (set_phase(&ipc, &lg, PHASE_END, 0) != 0) break;
            break;
        }

        // jesli stop przyszedl w trakcie rejsu -> konczymy po biezacym rejsie (jestesmy po doplynieciu)
        if (g_stop) {
            logf(&lg, "captain", "stop after trip completion -> END");
            if (set_phase(&ipc, &lg, PHASE_END, 0) != 0) break;
            break;
        }

        // przelacz kierunek na rejs powrotny
        if (sem_wait_nointr(ipc.sem_state) != 0) break;
        ipc.shm->direction = (ipc.shm->direction == DIR_KRAKOW_TO_TYNIEC)
            ? DIR_TYNIEC_TO_KRAKOW : DIR_KRAKOW_TO_TYNIEC;
        sem_post_chk(ipc.sem_state);
    }

    logf(&lg, "captain", "EXIT (g_exit=%d g_stop=%d g_early_depart=%d trips_done=%d)",
        (int)g_exit, (int)g_stop, (int)g_early_depart, (int)trips_done);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
