#include "cli.h"
#include "util.h"

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

int main(int argc, char** argv) {
    cli_args_t a;
    int r = cli_parse_dispatcher(argc, argv, &a);
    if (r == 1) { cli_print_usage_dispatcher(); return 0; }
    if (r != 0) { cli_print_usage_dispatcher(); return 2; }

    install_handlers();

    const pid_t captain_pid = a.captain_pid;

    fprintf(stderr,
        "Dispatcher pid=%d. Commands:\n"
        "  1 + ENTER -> send SIGUSR1 (early depart)\n"
        "  2 + ENTER -> send SIGUSR2 (stop)\n",
        (int)getpid());

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
        if (sel == 0) continue; // timeout

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
