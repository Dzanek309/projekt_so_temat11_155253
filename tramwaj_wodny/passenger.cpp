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
static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post");
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
    const int has_bike = (a.bike_flag == 1) ? 1 : 0; // w tej wersji zak³adamy, ¿e launcher podaje 0/1

    logf(&lg, "passenger",
        "started (pid=%d desired_dir=%d bike=%d)",
        (int)getpid(), desired_dir, has_bike);

    while (!g_exit) {
        sem_wait_nointr(ipc.sem_state);
        int shutdown = ipc.shm->shutdown;
        phase_t ph = ipc.shm->phase;
        sem_post_chk(ipc.sem_state);

        if (shutdown || ph == PHASE_END) {
            logf(&lg, "passenger", "observed shutdown/END -> exit");
            break;
        }

        usleep(100000); // 100ms
    }

    logf(&lg, "passenger", "EXIT (g_exit=%d)", (int)g_exit);
    logger_close(&lg);
    ipc_close(&ipc);
    return 0;
}
