#include "common.h"
#include "cli.h"
#include "util.h"
#include "logging.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

    install_handlers();

    fprintf(stderr, "tramwaj: ok (N=%d M=%d K=%d T1=%d T2=%d R=%d P=%d log=%s)\n",
        (int)args.N, (int)args.M, (int)args.K,
        (int)args.T1_ms, (int)args.T2_ms,
        (int)args.R, (int)args.P, args.log_path);

    // TODO: tworzenie IPC + spawn procesów w kolejnych commitach
    (void)g_shutdown;

    return 0;
}