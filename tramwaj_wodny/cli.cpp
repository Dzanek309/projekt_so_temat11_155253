#include "cli.h"
#include "util.h"
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int streq(const char* a, const char* b) { return strcmp(a, b) == 0; }
static int need_arg(int i, int argc) { return (i + 1) < argc; }

void cli_print_usage_tramwaj(void) {
    fprintf(stderr,
        "Usage:\n"
        "  tramwaj --N <int> --M <int> --K <int> --T1 <ms> --T2 <ms> --R <int> --P <int> [--bike-prob <0..1>] [--log <path>]\n"
        "Example:\n"
        "  ./tramwaj --N 20 --M 5 --K 6 --T1 1000 --T2 1500 --R 8 --P 60 --bike-prob 0.3 --log simulation.log\n");
}

void cli_print_usage_dispatcher(void) {
    fprintf(stderr,
        "Usage:\n"
        "  dispatcher --captain-pid <pid> --log <path>\n"
        "  (IPC args are optional for dispatcher)\n");
}

void cli_print_usage_captain(void) {
    fprintf(stderr,
        "Usage:\n"
        "  captain --shm <name> --sem-prefix <prefix> --msqid <id> --log <path>\n");
}

void cli_print_usage_passenger(void) {
    fprintf(stderr,
        "Usage:\n"
        "  passenger --shm <name> --sem-prefix <prefix> --msqid <id> --log <path> [--dir 0|1] [--bike 0|1]\n"
        "  dir: 0 Krakow->Tyniec, 1 Tyniec->Krakow\n");
}

static void init_defaults(cli_args_t* a) {
    memset(a, 0, sizeof(*a));
    a->bike_prob = 0.0;
    a->msqid = -1;
    a->captain_pid = -1;
    a->desired_dir = -1;
    a->bike_flag = -1;
    a->interactive = 1;
    snprintf(a->log_path, sizeof(a->log_path), "simulation.log");
}
