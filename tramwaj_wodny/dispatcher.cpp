#include "util.h"

#include <cstdlib>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
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

static void usage(void) {
    fprintf(stderr,
        "Usage (preferred / IPC):\n"
        "  dispatcher --shm <name> --sem-prefix <prefix> --msqid <id> --log <path>\n"
        "\n"
        "Usage (legacy):\n"
        "  dispatcher --captain-pid <pid> --log <path>\n"
    );
}

int main(int argc, char** argv) {
    install_handlers();

    const char* shm_name = NULL;
    const char* sem_prefix = NULL;
    const char* log_path = NULL;
    int msqid = -1;
    pid_t captain_pid = -1;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];

        auto need_val = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", opt);
                usage();
                _exit(2);
            }
            return argv[++i];
            };

        if (strcmp(a, "--shm") == 0) {
            shm_name = need_val("--shm");
        }
        else if (strcmp(a, "--sem-prefix") == 0) {
            sem_prefix = need_val("--sem-prefix");
        }
        else if (strcmp(a, "--msqid") == 0) {
            msqid = atoi(need_val("--msqid"));
        }
        else if (strcmp(a, "--log") == 0) {
            log_path = need_val("--log");
        }
        else if (strcmp(a, "--captain-pid") == 0) {
            captain_pid = (pid_t)atoi(need_val("--captain-pid"));
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        else {
            fprintf(stderr, "Unknown arg: %s\n", a);
            usage();
            return 2;
        }
    }

    if (!log_path) {
        fprintf(stderr, "dispatcher: missing --log\n");
        usage();
        return 2;
    }

    const int have_ipc = (shm_name && sem_prefix && msqid >= 0);

    if (!have_ipc && captain_pid <= 0) {
        fprintf(stderr, "dispatcher: need either IPC args or --captain-pid\n");
        usage();
        return 2;
    }

    fprintf(stderr,
        "dispatcher: ok (mode=%s, msqid=%d, captain_pid=%d)\n",
        have_ipc ? "IPC" : "LEGACY",
        msqid,
        (int)captain_pid
    );

    fprintf(stderr,
        "Dispatcher pid=%d. Commands:\n"
        "  1 + ENTER -> send SIGUSR1 (early depart)\n"
        "  2 + ENTER -> send SIGUSR2 (stop)\n",
        (int)getpid()
    );

    while (!g_exit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 0.2s

        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;

        char cmd = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') continue;
            cmd = buf[i];
            break;
        }

        if (cmd == '1') {
            if (captain_pid <= 1) {
                fprintf(stderr, "dispatcher: captain_pid unknown (provide --captain-pid)\n");
                continue;
            }
            if (kill(captain_pid, SIGUSR1) != 0) {
                perror("kill(SIGUSR1)");
            }
        }
        else if (cmd == '2') {
            if (captain_pid <= 1) {
                fprintf(stderr, "dispatcher: captain_pid unknown (provide --captain-pid)\n");
                continue;
            }
            if (kill(captain_pid, SIGUSR2) != 0) {
                perror("kill(SIGUSR2)");
            }
        }
    }

    return 0;
}
