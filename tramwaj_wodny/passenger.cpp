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

static volatile sig_atomic_t g_exit = 0;
static void on_term(int) { g_exit = 1; }

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);

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
    // EAGAIN/EINTR -> traktujemy jako "nie uda³o siê"
    if (sem_trywait(s) != 0) return -1;
    return 0;
}
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post");
}

static int desired_dir_ok(const shm_state_t* s, int desired_dir) {
    if (desired_dir < 0) return 1;
    return (int)s->direction == desired_dir;
}

static void release_n(sem_t* s, int n) {
    for (int i = 0; i < n; i++) sem_post_chk(s);
}

// Dziêki temu proces nie blokuje siê trzymaj¹c 1 jednostkê i czekaj¹c na drug¹.
static int acquire_units_atomic(sem_t* s, int units) {
    for (;;) {
        if (g_exit) return -1;

        int got = 0;
        for (int i = 0; i < units; i++) {
            if (sem_trywait_chk(s) != 0) {
                // rollback czêœciowego zajêcia
                if (got > 0) release_n(s, got);
                got = -1;
                break;
            }
            got++;
        }

        if (got == units) return units;
        sleep_ms(5);
    }
}

static void passenger_send_ack(ipc_handles_t* ipc, int trip_no) {
    msg_ack_t ack;
    ack.mtype = 1; // CAPTAIN mtype
    ack.pid = getpid();
    ack.trip_no = trip_no;

    if (msgsnd(ipc->msqid, &ack, sizeof(ack) - sizeof(long), 0) != 0) {
        perror("msgsnd(ACK)");
    }
}

static int read_trip_no(ipc_handles_t* ipc) {
    sem_wait_nointr(ipc->sem_state);
    int t = ipc->shm->trip_no;
    sem_post_chk(ipc->sem_state);
    return t;
}

// Obs³uga wymuszonego zejœcia w kolejnoœci LIFO:
// - czekamy a¿ dir=OUT
// - czekamy a¿ bêdziemy na back
// - pop_back
// - zwalniamy mostek + rezerwacje
static void passenger_handle_evict(ipc_handles_t* ipc, logger_t* lg,
    int units, int has_bike, int trip_no) {
    logf(lg, "passenger", "evict handling start (trip=%d)", trip_no);

    // czekaj a¿ kapitan ustawi dir OUT
    for (;;) {
        if (g_exit) return;

        sem_wait_nointr(ipc->sem_state);
        bridge_dir_t d = ipc->shm->bridge.dir;
        sem_post_chk(ipc->sem_state);

        if (d != BRIDGE_DIR_OUT) {
            sleep_ms(10);
            continue;
        }

        // czekamy a¿ bêdziemy na back (LIFO)
        for (;;) {
            if (g_exit) return;

            sem_wait_nointr(ipc->sem_state);
            bridge_node_t* b = bridge_back(ipc->shm);

            if (b && b->pid == getpid()) {
                bridge_node_t out;
                bridge_pop_back(ipc->shm, &out);

                if (ipc->shm->bridge.count == 0) ipc->shm->bridge.dir = BRIDGE_DIR_NONE;

                sem_post_chk(ipc->sem_state);

                // zwolnij zasoby (mostek + rezerwacje statku)
                release_n(ipc->sem_bridge, units);
                sem_post_chk(ipc->sem_seats);
                if (has_bike) sem_post_chk(ipc->sem_bikes);

                passenger_send_ack(ipc, trip_no);
                logf(lg, "passenger", "left bridge due to evict (LIFO), trip=%d", trip_no);
                return;
            }

            sem_post_chk(ipc->sem_state);
            sleep_ms(10);
        }
    }
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
    const int desired_dir = a.desired_dir;
    const int has_bike = (a.bike_flag == 1) ? 1 : 0;
    const int units = has_bike ? 2 : 1;

    logf(&lg, "passenger", "start desired_dir=%d bike=%d units=%d",
        desired_dir, has_bike, units);

    // Stan lokalny, ¿eby na wyjœciu nie dublowaæ zwolnieñ
    bool seat_reserved = false;
    bool bike_reserved = false;
    int  bridge_units_held = 0;   // ile jednostek mostka trzymamy (0/1/2)
    bool onboard_counted = false; // czy zwiêkszyliœmy onboard_* w SHM

    // Pasa¿er próbuje wejœæ tylko raz (jeœli siê nie uda - koñczy po czasie).
    const int64_t kGiveUpMs = 15000; // 15s
    int boarded = 0;
    const int64_t start = now_ms_monotonic();

    while (!g_exit) {
        // timeout na próbê wejœcia (zanim wejdziemy na statek)
        if (!boarded && (now_ms_monotonic() - start >= kGiveUpMs)) {
            logf(&lg, "passenger", "give up after %lldms without boarding", (long long)kGiveUpMs);
            break;
        }

        // odbierz ewentualne CMD_EVICT (nieblokuj¹co)
        msg_cmd_t cmd;
        ssize_t n = msgrcv(ipc.msqid, &cmd, sizeof(cmd) - sizeof(long), (long)me, IPC_NOWAIT);
        if (n >= 0 && cmd.cmd == CMD_EVICT) {
            passenger_handle_evict(&ipc, &lg, units, has_bike, cmd.trip_no);

            // po evict zwolniliœmy zasoby – lokalnie te¿ zerujemy
            seat_reserved = false;
            bike_reserved = false;
            bridge_units_held = 0;
            onboard_counted = false;
            goto finish;
        }

        // odczytaj stan (snapshot)
        sem_wait_nointr(ipc.sem_state);
        shm_state_t snapshot = *ipc.shm;
        sem_post_chk(ipc.sem_state);

        if (snapshot.shutdown || snapshot.phase == PHASE_END) {
            logf(&lg, "passenger", "END/shutdown observed -> exit");
            break;
        }

        // czekaj na LOADING w swoim kierunku
        if (snapshot.phase != PHASE_LOADING ||
            snapshot.boarding_open == 0 ||
            !desired_dir_ok(&snapshot, desired_dir)) {
            sleep_ms(20);
            continue;
        }

        // Spróbuj zarezerwowaæ miejsce na statku
        if (!seat_reserved) {
            if (sem_trywait_chk(ipc.sem_seats) != 0) {
                sleep_ms(10);
                continue;
            }
            seat_reserved = true;
        }

        if (has_bike && !bike_reserved) {
            if (sem_trywait_chk(ipc.sem_bikes) != 0) {
                sem_post_chk(ipc.sem_seats);
                seat_reserved = false;
                sleep_ms(10);
                continue;
            }
            bike_reserved = true;
        }

        // Spróbuj zarezerwowaæ jednostki mostka
        if (bridge_units_held == 0) {
            int got = 0;
            for (int i = 0; i < units; i++) {
                if (sem_trywait_chk(ipc.sem_bridge) != 0) break;
                got++;
            }
            bridge_units_held = got;

            if (bridge_units_held != units) {
                // rollback
                release_n(ipc.sem_bridge, bridge_units_held);
                bridge_units_held = 0;

                sem_post_chk(ipc.sem_seats);
                seat_reserved = false;

                if (has_bike) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
                sleep_ms(10);
                continue;
            }
        }

        // Wejœcie na mostek: wymagamy dir NONE lub IN
        sem_wait_nointr(ipc.sem_state);

        if (ipc.shm->phase != PHASE_LOADING ||
            ipc.shm->boarding_open == 0 ||
            !desired_dir_ok(ipc.shm, desired_dir)) {
            sem_post_chk(ipc.sem_state);

            // rollback
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            sem_post_chk(ipc.sem_seats);
            seat_reserved = false;

            if (has_bike) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
            sleep_ms(10);
            continue;
        }

        if (!(ipc.shm->bridge.dir == BRIDGE_DIR_NONE || ipc.shm->bridge.dir == BRIDGE_DIR_IN)) {
            sem_post_chk(ipc.sem_state);

            // rollback
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            sem_post_chk(ipc.sem_seats);
            seat_reserved = false;

            if (has_bike) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
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

            sem_post_chk(ipc.sem_seats);
            seat_reserved = false;

            if (has_bike) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }
            sleep_ms(10);
            continue;
        }

        sem_post_chk(ipc.sem_state);

        logf(&lg, "passenger", "entered bridge (dir IN), waiting to board");

        // Symulacja przejœcia
        sleep_ms(30);

        // Czekaj a¿ bêdziesz z przodu i boarding wci¹¿ otwarty
        for (;;) {
            if (g_exit) goto finish;

            // odbierz CMD_EVICT
            ssize_t n2 = msgrcv(ipc.msqid, &cmd, sizeof(cmd) - sizeof(long), (long)me, IPC_NOWAIT);
            if (n2 >= 0 && cmd.cmd == CMD_EVICT) {
                passenger_handle_evict(&ipc, &lg, units, has_bike, cmd.trip_no);

                seat_reserved = false;
                bike_reserved = false;
                bridge_units_held = 0;
                onboard_counted = false;
                goto finish;
            }

            sem_wait_nointr(ipc.sem_state);

            // jeœli boarding zamkniêty albo faza != LOADING -> zejdŸ LIFO
            if (ipc.shm->phase != PHASE_LOADING || ipc.shm->boarding_open == 0) {
                sem_post_chk(ipc.sem_state);

                const int trip = read_trip_no(&ipc);
                passenger_handle_evict(&ipc, &lg, units, has_bike, trip);

                seat_reserved = false;
                bike_reserved = false;
                bridge_units_held = 0;
                onboard_counted = false;
                goto finish;
            }

            bridge_node_t* fr = bridge_front(ipc.shm);
            if (fr && fr->pid == me && fr->evicting == 0) {
                // wchodzê na statek
                bridge_node_t out;
                bridge_pop_front(ipc.shm, &out);
                if (ipc.shm->bridge.count == 0) ipc.shm->bridge.dir = BRIDGE_DIR_NONE;

                ipc.shm->onboard_passengers += 1;
                if (has_bike) ipc.shm->onboard_bikes += 1;

                const int onboard = ipc.shm->onboard_passengers;
                const int bikes = ipc.shm->onboard_bikes;

                sem_post_chk(ipc.sem_state);

                // zszed³em z mostka -> zwalniam jednostki mostka
                release_n(ipc.sem_bridge, bridge_units_held);
                bridge_units_held = 0;

                onboard_counted = true;
                boarded = 1;

                logf(&lg, "passenger", "BOARDED ship (onboard=%d bikes=%d)", onboard, bikes);
                break;
            }

            sem_post_chk(ipc.sem_state);
            sleep_ms(10);
        }

        break; // po wejœciu/odmowie koñczymy próbê
    }

    if (!boarded) {
        logf(&lg, "passenger", "did not board (timeout or shutdown)");
        goto finish;
    }

    // Czekaj na UNLOADING i wyjdŸ ze statku (DIR_OUT)
    while (!g_exit) {
        sem_wait_nointr(ipc.sem_state);
        phase_t ph = ipc.shm->phase;
        int shutdown = ipc.shm->shutdown;
        sem_post_chk(ipc.sem_state);

        if (shutdown || ph == PHASE_END) goto finish;
        if (ph == PHASE_UNLOADING) break;
        sleep_ms(20);
    }

    // zejœcie: zajmij mostek units
    // ===== FIX: atomowo (trywait+rollback), ¿eby nie blokowaæ siê trzymaj¹c 1/2 zasobu =====
    {
        int gotu = acquire_units_atomic(ipc.sem_bridge, units);
        if (gotu < 0) goto finish;
        bridge_units_held = gotu;
    }

    sem_wait_nointr(ipc.sem_state);
    if (ipc.shm->bridge.dir == BRIDGE_DIR_NONE) ipc.shm->bridge.dir = BRIDGE_DIR_OUT;

    // wejœcie od strony statku
    bridge_node_t node2;
    node2.pid = me;
    node2.units = (uint8_t)units;
    node2.evicting = 0;
    (void)bridge_push_front(ipc.shm, node2);
    sem_post_chk(ipc.sem_state);

    sleep_ms(30);

    // zejœcie na l¹d: tylko back w DIR_OUT
    for (;;) {
        if (g_exit) goto finish;

        sem_wait_nointr(ipc.sem_state);
        bridge_node_t* bk = bridge_back(ipc.shm);
        if (bk && bk->pid == me) {
            bridge_node_t out;
            bridge_pop_back(ipc.shm, &out);
            if (ipc.shm->bridge.count == 0) ipc.shm->bridge.dir = BRIDGE_DIR_NONE;

            ipc.shm->onboard_passengers -= 1;
            if (has_bike) ipc.shm->onboard_bikes -= 1;

            sem_post_chk(ipc.sem_state);

            // zwolnij mostek
            release_n(ipc.sem_bridge, bridge_units_held);
            bridge_units_held = 0;

            // zwolnij miejsce na statku i rower
            if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }
            if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }

            onboard_counted = false;
            logf(&lg, "passenger", "LEFT ship and freed resources");
            break;
        }

        sem_post_chk(ipc.sem_state);
        sleep_ms(10);
    }

finish:
    // Best-effort cleanup (¿eby nie zostawiæ zasobów przy SIGTERM)
    if (bridge_units_held > 0) {
        release_n(ipc.sem_bridge, bridge_units_held);
        bridge_units_held = 0;
    }

    if (onboard_counted) {
        // awaryjnie skoryguj liczniki (¿eby kapitan nie wisia³)
        sem_wait_nointr(ipc.sem_state);
        if (ipc.shm->onboard_passengers > 0) ipc.shm->onboard_passengers -= 1;
        if (has_bike && ipc.shm->onboard_bikes > 0) ipc.shm->onboard_bikes -= 1;
        sem_post_chk(ipc.sem_state);
        onboard_counted = false;
    }

    if (seat_reserved) { sem_post_chk(ipc.sem_seats); seat_reserved = false; }
    if (bike_reserved) { sem_post_chk(ipc.sem_bikes); bike_reserved = false; }

    // log zakoñczenia procesu pasa¿era
    logf(&lg, "passenger",
        "EXIT (boarded=%d exit_flag=%d)",
        boarded, (int)g_exit);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
