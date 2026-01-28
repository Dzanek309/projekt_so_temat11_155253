#ifndef IPC_H
#define IPC_H

#include "common.h"
#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        // nazwy (u¿ywane do cleanup)
        char shm_name[128];
        char sem_prefix[128];

        // uchwyty
        int shm_fd;
        shm_state_t* shm;

        sem_t* sem_state;   // mutex do SHM
        sem_t* sem_log;     // mutex do logu
        sem_t* sem_seats;   // N
        sem_t* sem_bikes;   // M
        sem_t* sem_bridge;  // K (units)

        int msqid;          // SysV message queue id
    } ipc_handles_t;

    // Tworzy IPC (tylko launcher)
    int ipc_create(ipc_handles_t* h, const char* shm_name, const char* sem_prefix,
        const shm_state_t* initial_state, int* out_msqid);

    // Otwiera IPC (dzieci)
    int ipc_open(ipc_handles_t* h, const char* shm_name, const char* sem_prefix, int msqid);

    // Zamkniêcie (wszyscy)
    void ipc_close(ipc_handles_t* h);

    // Cleanup (tylko launcher): sem_unlink/shm_unlink/msgctl(IPC_RMID)
    int ipc_destroy(const char* shm_name, const char* sem_prefix, int msqid);

#ifdef __cplusplus
}
#endif

#endif // IPC_H