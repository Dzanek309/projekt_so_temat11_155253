#include "common.h"
#include "ipc.h"
#include "cli.h"
#include "logging.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_shutdown = 0;
static void on_term(int) { g_shutdown = 1; }

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)");
}

int main(int argc, char** argv) {
    cli_args_t args;
    int pr = cli_parse_launcher(argc, argv, &args);
    if (pr == 1) return 0;
    if (pr != 0) { cli_print_usage_tramwaj(); return 2; }

    char err[128];
    if (cli_validate_launcher(&args, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "Invalid args: %s\n", err);
        return 2;
    }

    // Minimalne prawa dostępu do IPC
    umask(0077);

    install_handlers();

    pid_t launcher_pid = getpid();
    char shm_name[128];
    char sem_prefix[128];
    snprintf(shm_name, sizeof(shm_name), "/tramwaj_shm_%d", (int)launcher_pid);
    snprintf(sem_prefix, sizeof(sem_prefix), "/tramwaj_%d", (int)launcher_pid);

    shm_state_t init;
    memset(&init, 0, sizeof(init));
    init.N = args.N;
    init.M = args.M;
    init.K = args.K;
    init.T1_ms = args.T1_ms;
    init.T2_ms = args.T2_ms;
    init.R = args.R;
    init.P = args.P;

    init.phase = PHASE_LOADING;
    init.direction = DIR_KRAKOW_TO_TYNIEC;
    init.boarding_open = 1;
    init.trip_no = 0;
    init.shutdown = 0;

    init.onboard_passengers = 0;
    init.onboard_bikes = 0;

    init.bridge.dir = BRIDGE_DIR_NONE;
    init.bridge.load_units = 0;
    init.bridge.count = 0;
    init.bridge.head = 0;
    init.bridge.tail = 0;

    ipc_handles_t ipc;
    int msqid = -1;
    if (ipc_create(&ipc, shm_name, sem_prefix, &init, &msqid) != 0) {
        fprintf(stderr, "tramwaj: ipc_create failed\n");
        return 1;
    }

    logger_t lg;
    if (logger_open(&lg, args.log_path, ipc.sem_log) != 0) {
        fprintf(stderr, "tramwaj: logger_open failed\n");
        ipc_close(&ipc);
        ipc_destroy(shm_name, sem_prefix, msqid);
        return 1;
    }

    logf(&lg, "launcher", "IPC created shm=%s sem_prefix=%s msqid=%d (skeleton)",
        shm_name, sem_prefix, msqid);

    // na razie tylko szkielet: tworzymy IPC i od razu kończymy
    if (g_shutdown) {
        logf(&lg, "launcher", "shutdown requested early");
    }

    logf(&lg, "launcher", "cleaning up IPC (skeleton)");
    logger_close(&lg);

    ipc_close(&ipc);
    ipc_destroy(shm_name, sem_prefix, msqid);
    return 0;
}
