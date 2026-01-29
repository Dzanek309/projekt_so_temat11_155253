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
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post");
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

    while (!g_exit) {
        // Obserwuj globalny shutdown
        sem_wait_nointr(ipc.sem_state);
        int shutdown = ipc.shm->shutdown;
        int t1 = ipc.shm->T1_ms;
        sem_post_chk(ipc.sem_state);

        if (shutdown) {
            logf(&lg, "captain", "shutdown flag set -> END");
            set_phase(&ipc, &lg, PHASE_END, 0);
            break;
        }

        // nowy cykl: LOADING
        g_early_depart = 0;
        set_phase(&ipc, &lg, PHASE_LOADING, 1);

        int64_t start = now_ms_monotonic();
        while (!g_exit) {
            if (g_stop) break;
            if (g_early_depart) break;
            if (now_ms_monotonic() - start >= (int64_t)t1) break;
            sleep_ms(20);
        }

        // na razie: tylko domkniêcie cyklu
        set_phase(&ipc, &lg, PHASE_DEPARTING, 0);

        if (g_stop) {
            logf(&lg, "captain", "stop received during LOADING -> END (stub)");
            set_phase(&ipc, &lg, PHASE_END, 0);
            break;
        }

        logf(&lg, "captain", "departing after LOADING -> END (stub)");
        set_phase(&ipc, &lg, PHASE_END, 0);
        break;
    }

    logf(&lg, "captain", "EXIT (g_exit=%d g_stop=%d g_early_depart=%d)",
        (int)g_exit, (int)g_stop, (int)g_early_depart);

    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
