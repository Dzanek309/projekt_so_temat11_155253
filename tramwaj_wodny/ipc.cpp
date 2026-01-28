#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int ipc_create(ipc_handles_t * h, const char* shm_name, const char* sem_prefix,
}

int ipc_open(ipc_handles_t* h, const char* shm_name, const char* sem_prefix, int msqid) {
    return 0;
}

void ipc_close(ipc_handles_t* h) {
}

int ipc_destroy(const char* shm_name, const char* sem_prefix, int msqid) {
}