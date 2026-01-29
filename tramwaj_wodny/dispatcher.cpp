#include "util.h"

#include <cstdlib>
#include <errno.h>
#include <signal.h>
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

    // na razie tylko szkielet; pêtla/IPC/log w kolejnych commitach
    while (!g_exit) {
        usleep(100000);
        break;
    }

    return 0;
}
