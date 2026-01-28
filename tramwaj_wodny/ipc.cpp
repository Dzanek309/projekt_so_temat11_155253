#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int ipc_create(ipc_handles_t * h, const char* shm_name, const char* sem_prefix,
    const shm_state_t * initial_state, int* out_msqid) {
    if (!h || !shm_name || !sem_prefix || !initial_state || !out_msqid) return -1;
    memset(h, 0, sizeof(*h));
    snprintf(h->shm_name, sizeof(h->shm_name), "%s", shm_name);
    snprintf(h->sem_prefix, sizeof(h->sem_prefix), "%s", sem_prefix);
    h->shm_fd = -1;
    h->shm = NULL;
    h->msqid = -1;
    *out_msqid = -1;

    // SHM
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    h->shm_fd = fd;

    if (ftruncate(fd, (off_t)sizeof(shm_state_t)) != 0) { perror("ftruncate"); return -1; }

    void* p = mmap(NULL, sizeof(shm_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }
    h->shm = (shm_state_t*)p;
    memcpy(h->shm, initial_state, sizeof(shm_state_t));

    return 0;
}

int ipc_open(ipc_handles_t* h, const char* shm_name, const char* sem_prefix, int msqid) {
    if (!h || !shm_name || !sem_prefix) return -1;
    memset(h, 0, sizeof(*h));
    snprintf(h->shm_name, sizeof(h->shm_name), "%s", shm_name);
    snprintf(h->sem_prefix, sizeof(h->sem_prefix), "%s", sem_prefix);
    h->shm_fd = -1;
    h->shm = NULL;
    h->msqid = msqid;

    int fd = shm_open(shm_name, O_RDWR, 0600);
    if (fd < 0) { perror("shm_open(open)"); return -1; }
    h->shm_fd = fd;

    void* p = mmap(NULL, sizeof(shm_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap(open)"); return -1; }
    h->shm = (shm_state_t*)p;

    return 0;
}

void ipc_close(ipc_handles_t* h) {
    if (!h) return;
    if (h->shm && h->shm != MAP_FAILED) munmap(h->shm, sizeof(shm_state_t));
    h->shm = NULL;
    if (h->shm_fd >= 0) close(h->shm_fd);
    h->shm_fd = -1;
}

int ipc_destroy(const char* shm_name, const char* sem_prefix, int msqid) {
    (void)sem_prefix;
    (void)msqid;
    if (!shm_name) return -1;
    if (shm_unlink(shm_name) != 0) perror("shm_unlink");
    return 0;
}