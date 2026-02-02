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
}

static void sem_wait_nointr(sem_t* s) {
    while (sem_wait(s) != 0) {
        if (errno == EINTR) continue;
        die_perror("sem_wait");
    }
}
static void sem_post_chk(sem_t* s) { if (sem_post(s) != 0) die_perror("sem_post"); }

static const char* dir_str(int d) {
    return (d == DIR_KRAKOW_TO_TYNIEC) ? "KRAKOW->TYNIEC" : "TYNIEC->KRAKOW";
}

// Kapitan wymusza zejœcie od koñca kolejki (LIFO) poprzez:
// - ustawienie phase=DEPARTING i boarding_open=0
// - ustawienie bridge.dir = OUT
// - pêtla: wybierz back, oznacz evicting, wyœlij CMD_EVICT(pid), czekaj na ACK
// Dodatkowo: zliczamy ile OSÓB zesz³o z mostka (ile evictów).
static void captain_clear_bridge(ipc_handles_t* ipc, logger_t* lg, int* out_left_bridge_people) {
    int left_cnt = 0;

    for (;;) {
        sem_wait_nointr(ipc->sem_state);

        if (ipc->shm->bridge.count == 0) {
            ipc->shm->bridge.dir = BRIDGE_DIR_NONE;
            sem_post_chk(ipc->sem_state);
            logf(lg, "captain", "bridge empty -> ok to depart");
            if (out_left_bridge_people) *out_left_bridge_people = left_cnt;
            return;
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

        // wyœlij polecenie ewakuacji do konkretnego PID (mtype=PID)
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
                if (errno == EINTR) continue;
                perror("msgrcv(ACK)");
                break;
            }
            if (ack.pid == target) {
                left_cnt++;
                logf(lg, "captain", "ack from pid=%d (left_bridge=%d)", (int)target, left_cnt);
                break;
            }
            // Jeœli przyjdzie inny ack, ignorujemy (w praktyce rzadkie przy sekwencyjnym evict).
        }
    }
}

static void set_phase(ipc_handles_t* ipc, logger_t* lg, phase_t ph, int boarding_open) {
    sem_wait_nointr(ipc->sem_state);
    ipc->shm->phase = ph;
    ipc->shm->boarding_open = boarding_open;
    sem_post_chk(ipc->sem_state);
    logf(lg, "captain", "phase=%d boarding_open=%d", (int)ph, boarding_open);
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

    // ====== STUB: cykle rejsów, zakoñcz po R ======
    while (!g_exit) {
        // SprawdŸ shutdown z launchera
        sem_wait_nointr(ipc.sem_state);
        int shutdown = ipc.shm->shutdown;
        int t1 = ipc.shm->T1_ms;
        int t2 = ipc.shm->T2_ms;
        int rmax = ipc.shm->R;
        sem_post_chk(ipc.sem_state);

        if (shutdown) {
            logf(&lg, "captain", "shutdown flag set -> END");
            set_phase(&ipc, &lg, PHASE_END, 0);
            break;
        }

        // reset jednorazowego sygna³u "early depart" na start tripu
        g_early_depart = 0;

        // ===== LOADING =====
        set_phase(&ipc, &lg, PHASE_LOADING, 1);

        sem_wait_nointr(ipc.sem_state);
        ipc.shm->trip_no += 1;
        int my_trip = ipc.shm->trip_no;

        // snapshot kierunku dla statystyk tej podró¿y
        int trip_dir = (int)ipc.shm->direction;

        ipc.shm->bridge.dir = BRIDGE_DIR_NONE;
        sem_post_chk(ipc.sem_state);

        logf(&lg, "captain", "trip=%d direction=%d LOADING", my_trip, trip_dir);

        int64_t start = now_ms_monotonic();
        while (!g_exit) {
            // jeœli sygna³2 dotar³ w trakcie za³adunku: statek nie wyp³ywa, pasa¿erowie opuszczaj¹ statek
            if (g_stop) {
                logf(&lg, "captain", "stop during LOADING -> cancel trip and UNLOADING");
                break;
            }
            if (g_early_depart) {
                logf(&lg, "captain", "early depart signal received");
                break;
            }
            if (now_ms_monotonic() - start >= (int64_t)t1) {
                logf(&lg, "captain", "T1 elapsed -> depart");
                break;
            }
            sleep_ms(20);
        }

        // Zamknij boarding i przejdŸ do DEPARTING
        set_phase(&ipc, &lg, PHASE_DEPARTING, 0);

        // wymuœ ruch OUT na mostku (¿eby nikt nie wchodzi³ na statek)
        sem_wait_nointr(ipc.sem_state);
        ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
        sem_post_chk(ipc.sem_state);

        // wyczyœæ mostek od koñca (LIFO) + policz ile osób zesz³o z mostka
        int trip_left_bridge = 0;
        captain_clear_bridge(&ipc, &lg, &trip_left_bridge);

        // snapshot: ilu faktycznie pop³ynie w tej podró¿y (po zamkniêciu boardingu i oczyszczeniu mostka)
        int trip_boarded_pax = 0;
        int trip_boarded_bikes = 0;
        sem_wait_nointr(ipc.sem_state);
        trip_boarded_pax = ipc.shm->onboard_passengers;
        trip_boarded_bikes = ipc.shm->onboard_bikes;
        sem_post_chk(ipc.sem_state);

        if (g_stop) {
            // roz³adunek na miejscu (statek nie wyp³ywa)
            set_phase(&ipc, &lg, PHASE_UNLOADING, 0);

            // pozwól pasa¿erom zejœæ (kierunek mostka OUT)
            sem_wait_nointr(ipc.sem_state);
            ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
            sem_post_chk(ipc.sem_state);

            // czekaj a¿ wszyscy zejda
            for (;;) {
                sem_wait_nointr(ipc.sem_state);
                int onboard = ipc.shm->onboard_passengers;
                sem_post_chk(ipc.sem_state);
                if (onboard == 0) break;
                sleep_ms(50);
            }

            logf(&lg, "captain", "unloading complete (stop)");
            logf(&lg, "captain",
                "TRIP SUMMARY trip=%d route=%s passengers=%d bikes=%d left_bridge=%d",
                my_trip, dir_str(trip_dir), trip_boarded_pax, trip_boarded_bikes, trip_left_bridge);

            logf(&lg, "captain", "all passengers left after stop -> END");
            set_phase(&ipc, &lg, PHASE_END, 0);
            break;
        }

        // ===== SAILING =====
        set_phase(&ipc, &lg, PHASE_SAILING, 0);
        logf(&lg, "captain", "sailing for T2=%dms", t2);
        int64_t sail_start = now_ms_monotonic();
        while (!g_exit) {
            if (now_ms_monotonic() - sail_start >= (int64_t)t2) break;
            sleep_ms(20);
        }

        // ===== UNLOADING =====
        set_phase(&ipc, &lg, PHASE_UNLOADING, 0);
        sem_wait_nointr(ipc.sem_state);
        ipc.shm->bridge.dir = BRIDGE_DIR_OUT;
        sem_post_chk(ipc.sem_state);
        logf(&lg, "captain", "arrived -> UNLOADING");

        // czekaj a¿ wszyscy zejda
        for (;;) {
            sem_wait_nointr(ipc.sem_state);
            int onboard = ipc.shm->onboard_passengers;
            sem_post_chk(ipc.sem_state);
            if (onboard == 0) break;
            sleep_ms(50);
        }

        logf(&lg, "captain", "unloading complete");
        logf(&lg, "captain",
            "TRIP SUMMARY trip=%d route=%s passengers=%d bikes=%d left_bridge=%d",
            my_trip, dir_str(trip_dir), trip_boarded_pax, trip_boarded_bikes, trip_left_bridge);

        trips_done++;
        if (trips_done >= rmax) {
            logf(&lg, "captain", "max trips R=%d reached -> END", rmax);
            set_phase(&ipc, &lg, PHASE_END, 0);
            break;
        }

        // prze³¹cz kierunek na rejs powrotny
        sem_wait_nointr(ipc.sem_state);
        ipc.shm->direction = (ipc.shm->direction == DIR_KRAKOW_TO_TYNIEC)
            ? DIR_TYNIEC_TO_KRAKOW : DIR_KRAKOW_TO_TYNIEC;
        sem_post_chk(ipc.sem_state);
    }

    logf(&lg, "captain", "EXIT (g_exit=%d g_stop=%d g_early_depart=%d)",
        (int)g_exit, (int)g_stop, (int)g_early_depart);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
