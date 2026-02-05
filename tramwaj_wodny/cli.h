#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        // parametry symulacji
        int32_t N, M, K;
        int32_t T1_ms, T2_ms;
        int32_t R;
        int32_t P;
        double bike_prob;

        // IPC
        char shm_name[128];
        char sem_prefix[128];
        int32_t msqid;

        // log
        char log_path[256];

        // role-specific
        pid_t captain_pid;      // tylko dispatcher
        int32_t desired_dir;    // tylko passenger (0/1), -1 random
        int32_t bike_flag;      // tylko passenger: -1 losuj, 0 bez, 1 z rowerem
        int32_t interactive;    // dispatcher
    } cli_args_t;

    // Parser uzywany przez rozne binarki.
    // W zaleznosci od programu wymagane jest podanie roznych pol.
    int cli_parse_launcher(int argc, char** argv, cli_args_t* out);
    int cli_parse_child_common(int argc, char** argv, cli_args_t* out); // shm/sem/msq/log
    int cli_parse_dispatcher(int argc, char** argv, cli_args_t* out);   // + captain_pid
    int cli_parse_passenger(int argc, char** argv, cli_args_t* out);    // + dir/bike

    int cli_validate_launcher(const cli_args_t* a, char* err, int err_sz);

    void cli_print_usage_tramwaj(void);
    void cli_print_usage_dispatcher(void);
    void cli_print_usage_captain(void);
    void cli_print_usage_passenger(void);

#ifdef __cplusplus
}
#endif

#endif // CLI_H
