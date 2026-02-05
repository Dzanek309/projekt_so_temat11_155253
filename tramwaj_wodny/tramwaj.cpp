#include "common.h"
#include "ipc.h"
#include "cli.h"
#include "util.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
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
    if (sigaction(SIGHUP, &sa, NULL) != 0) die_perror("sigaction(SIGHUP)");
}

static void spawn_exec(const char* path, char* const argvv[], pid_t* out_pid) {
    pid_t pid = fork();
    if (pid < 0) die_perror("fork");
    if (pid == 0) {
        execv(path, argvv);
        die_perror("execv");
    }
    if (out_pid) *out_pid = pid;
}

static int proc_limit_ok(int want_children) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NPROC, &rl) != 0) {
        perror("getrlimit(RLIMIT_NPROC)");
        return 1; // nie blokuj jeśli nie da się odczytać
    }
    // rl.rlim_cur może być RLIM_INFINITY
    if (rl.rlim_cur == RLIM_INFINITY) return 1;
    // Minimalny check: jeśli limit jest bardzo niski, ostrzeż.
    // W praktyce i tak fork() zwróci błąd.
    if ((unsigned long)want_children + 20 > (unsigned long)rl.rlim_cur) return 0;
    return 1;
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

    // Limit procesów: launcher + captain + dispatcher + P
    int want_children = 2 + args.P;
    if (!proc_limit_ok(want_children)) {
        fprintf(stderr, "Refusing to spawn %d children: RLIMIT_NPROC too low\n", want_children);
        return 2;
    }

    install_handlers();
    if (setpgid(0, 0) != 0) perror("setpgid(launcher)");
    pid_t sim_pgid = getpgrp();

    // Unikalne nazwy IPC zależne od PID launchera
    pid_t launcher_pid = getpid();
    char shm_name[128];
    char sem_prefix[128];
    snprintf(shm_name, sizeof(shm_name), "/tramwaj_shm_%d", (int)launcher_pid);
    snprintf(sem_prefix, sizeof(sem_prefix), "/tramwaj_%d", (int)launcher_pid);

    // Stan początkowy SHM
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
        fprintf(stderr, "Failed to create IPC\n");
        return 1;
    }

    int guard_pipe[2];
    if (pipe(guard_pipe) != 0) die_perror("pipe(guard_pipe)");
    fcntl(guard_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(guard_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t guardian_pid = fork();
    if (guardian_pid < 0) die_perror("fork(guardian)");

    if (guardian_pid == 0) {
        if (setpgid(0, 0) != 0) perror("setpgid(guardian)");
        close(guard_pipe[1]);
        char tmp;
        ssize_t n = read(guard_pipe[0], &tmp, 1);
        close(guard_pipe[0]);
        if (n == 1) _exit(0);
        kill(-sim_pgid, SIGTERM);
        usleep(300 * 1000);
        kill(-sim_pgid, SIGKILL);
        usleep(200 * 1000);
        ipc_destroy(shm_name, sem_prefix, msqid);
        _exit(0);
    }

    close(guard_pipe[0]);

    (void)unlink(args.log_path);
    logger_t lg;
    if (logger_open(&lg, args.log_path, ipc.sem_log) != 0) {
        fprintf(stderr, "Failed to open log\n");
        ipc_close(&ipc);
        close(guard_pipe[1]);
        return 1;
    }
    logf(&lg, "launcher", "IPC created shm=%s sem_prefix=%s msqid=%d", shm_name, sem_prefix, msqid);

    // Spawn captain
    char msqid_buf[32];
    snprintf(msqid_buf, sizeof(msqid_buf), "%d", msqid);

    char* captain_argv[] = {
      (char*)"./captain",
      (char*)"--shm", shm_name,
      (char*)"--sem-prefix", sem_prefix,
      (char*)"--msqid", msqid_buf,
      (char*)"--log", args.log_path,
      NULL
    };

    pid_t captain_pid = -1;
    spawn_exec("./captain", captain_argv, &captain_pid);
    logf(&lg, "launcher", "spawned captain pid=%d", (int)captain_pid);

    // Zapisz PID kapitana w SHM
    while (sem_wait(ipc.sem_state) != 0) { if (errno == EINTR) continue; die_perror("sem_wait"); }
    ipc.shm->captain_pid = captain_pid;
    if (sem_post(ipc.sem_state) != 0) die_perror("sem_post");

    // Spawn dispatcher
    char* dispatcher_argv[] = {
      (char*)"./dispatcher",
      (char*)"--shm", shm_name,
      (char*)"--sem-prefix", sem_prefix,
      (char*)"--msqid", msqid_buf,
      (char*)"--log", args.log_path,
      NULL
    };
    pid_t dispatcher_pid = -1;
    spawn_exec("./dispatcher", dispatcher_argv, &dispatcher_pid);
    logf(&lg, "launcher", "spawned dispatcher pid=%d", (int)dispatcher_pid);

    // Spawn passengers
    // Pasażerowie losują kierunek (0/1) i rower wg bike_prob
    srand((unsigned)launcher_pid);

    pid_t* passenger_pids = (pid_t*)calloc((size_t)args.P, sizeof(pid_t));
    if (!passenger_pids && args.P > 0) die_perror("calloc");

    for (int i = 0; i < args.P; i++) {
        if (g_shutdown) break;

        int dir = rand() % 2;
        double r01 = (double)rand() / (double)RAND_MAX;
        int bike = (r01 < args.bike_prob) ? 1 : 0;

        char dir_buf[8], bike_buf[8];
        snprintf(dir_buf, sizeof(dir_buf), "%d", dir);
        snprintf(bike_buf, sizeof(bike_buf), "%d", bike);

        char* pass_argv[] = {
          (char*)"./passenger",
          (char*)"--shm", shm_name,
          (char*)"--sem-prefix", sem_prefix,
          (char*)"--msqid", msqid_buf,
          (char*)"--log", args.log_path,
          (char*)"--dir", dir_buf,
          (char*)"--bike", bike_buf,
          NULL
        };

        pid_t pp = -1;
        spawn_exec("./passenger", pass_argv, &pp);
        passenger_pids[i] = pp;
    }

    // główna pętla czekania
    int alive = want_children;
    while (alive > 0) {
        if (g_shutdown) {
            logf(&lg, "launcher", "shutdown requested, signalling children...");

            if (captain_pid > 1) {
                if (kill(captain_pid, SIGTERM) != 0) perror("kill(SIGTERM captain)");
            }
            if (dispatcher_pid > 1) {
                if (kill(dispatcher_pid, SIGTERM) != 0) perror("kill(SIGTERM dispatcher)");
            }
            for (int i = 0; i < args.P; i++) {
                if (passenger_pids && passenger_pids[i] > 1) {
                    if (kill(passenger_pids[i], SIGTERM) != 0) perror("kill(SIGTERM passenger)");
                }
            }

            sleep_ms(500);

            if (captain_pid > 1) {
                if (kill(captain_pid, SIGKILL) != 0) perror("kill(SIGKILL captain)");
            }
            if (dispatcher_pid > 1) {
                if (kill(dispatcher_pid, SIGKILL) != 0) perror("kill(SIGKILL dispatcher)");
            }
            for (int i = 0; i < args.P; i++) {
                if (passenger_pids && passenger_pids[i] > 1) {
                    if (kill(passenger_pids[i], SIGKILL) != 0) perror("kill(SIGKILL passenger)");
                }
            }

            g_shutdown = 0;
        }

        int status = 0;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        alive--;
    }

    logf(&lg, "launcher (tramwaj)", "children finished, cleaning up IPC");
    logger_close(&lg);

    ipc_close(&ipc);
    ipc_destroy(shm_name, sem_prefix, msqid);
    free(passenger_pids);
    if (guard_pipe[1] >= 0) {
        char bye = 0;
        (void)write(guard_pipe[1], &bye, 1);
        close(guard_pipe[1]);
    }

    return 0;
}
