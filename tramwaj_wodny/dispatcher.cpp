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

    if (sigaction(SIGINT, &sa, NULL) != 0) die_perror("sigaction(SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) != 0) die_perror("sigaction(SIGTERM)");
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  dispatcher --captain-pid <pid>\n"
        "\n"
        "Commands (stdin):\n"
        "  1 + ENTER -> SIGUSR1 (early depart)\n"
        "  2 + ENTER -> SIGUSR2 (stop)\n");
}

static int parse_pid(int argc, char** argv, pid_t* out_pid) {
    pid_t pid = -1;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];

        if (strcmp(a, "--captain-pid") == 0) {
            if (i + 1 >= argc) return -1;
            pid = (pid_t)atoi(argv[++i]);
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 1; // help shown
        }
        else {
            fprintf(stderr, "Unknown arg: %s\n", a);
            return -1;
        }
    }

    if (pid <= 1) return -1;
    *out_pid = pid;
    return 0;
}

int main(int argc, char** argv) {
    install_handlers();

    pid_t captain_pid = -1;
    int pr = parse_pid(argc, argv, &captain_pid);
    if (pr == 1) return 0;
    if (pr != 0) {
        usage();
        return 2;
    }

    fprintf(stderr,
        "Dispatcher pid=%d (captain_pid=%d). Commands:\n"
        "  1 + ENTER -> send SIGUSR1 (early depart)\n"
        "  2 + ENTER -> send SIGUSR2 (stop)\n",
        (int)getpid(), (int)captain_pid);

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
            if (kill(captain_pid, SIGUSR1) != 0) perror("kill(SIGUSR1)");
        }
        else if (cmd == '2') {
            if (kill(captain_pid, SIGUSR2) != 0) perror("kill(SIGUSR2)");
        }
    }

    return 0;
}
