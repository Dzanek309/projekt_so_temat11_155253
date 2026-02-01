#include "cli.h"
#include "util.h"

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

int main(int argc, char** argv) {
    cli_args_t a;
    int r = cli_parse_passenger(argc, argv, &a);
    if (r == 1) { cli_print_usage_passenger(); return 0; }
    if (r != 0) { cli_print_usage_passenger(); return 2; }

    install_handlers();

    fprintf(stderr,
        "passenger: start (shm=%s sem_prefix=%s msqid=%d dir=%d bike=%d log=%s)\n",
        a.shm_name, a.sem_prefix, (int)a.msqid, (int)a.desired_dir, (int)a.bike_flag, a.log_path);

    while (!g_exit) {
        usleep(100000);
        break;
    }

    return 0;
}
