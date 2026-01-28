#include "ipc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void build_sem_name(char* out, size_t out_sz, const char* prefix, const char* suffix) {
    // sem name must start with '/'
    snprintf(out, out_sz, "%s_%s", prefix, suffix);
}

static sem_t* sem_open_create(const char* name, unsigned init_val) {
    sem_t* s = sem_open(name, O_CREAT | O_EXCL, 0600, init_val);
    if (s == SEM_FAILED) {
        perror("sem_open(create)");
        return SEM_FAILED;
    }
    return s;
}

static sem_t* sem_open_existing(const char* name) {
    sem_t* s = sem_open(name, 0);
    if (s == SEM_FAILED) {
        perror("sem_open(open)");
        return SEM_FAILED;
    }
    return s;
}

int ipc_create(ipc_handles_t* h, const char* shm_name, const char* sem_prefix,
    const shm_state_t* initial_state, int* out_msqid) {
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

    // Semafory: state + log
    char name[256];

    build_sem_name(name, sizeof(name), sem_prefix, "state");
    h->sem_state = sem_open_create(name, 1);
    if (h->sem_state == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "log");
    h->sem_log = sem_open_create(name, 1);
    if (h->sem_log == SEM_FAILED) return -1;

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

    char name[256];
    build_sem_name(name, sizeof(name), sem_prefix, "state");
    h->sem_state = sem_open_existing(name);
    if (h->sem_state == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "log");
    h->sem_log = sem_open_existing(name);
    if (h->sem_log == SEM_FAILED) return -1;

    return 0;
}

void ipc_close(ipc_handles_t* h) {
    if (!h) return;

    if (h->shm && h->shm != MAP_FAILED) munmap(h->shm, sizeof(shm_state_t));
    h->shm = NULL;
    if (h->shm_fd >= 0) close(h->shm_fd);
    h->shm_fd = -1;

    if (h->sem_state && h->sem_state != SEM_FAILED) sem_close(h->sem_state);
    if (h->sem_log && h->sem_log != SEM_FAILED) sem_close(h->sem_log);
    h->sem_state = h->sem_log = SEM_FAILED;
}

int ipc_destroy(const char* shm_name, const char* sem_prefix, int msqid) {
    (void)msqid;
    if (!shm_name || !sem_prefix) return -1;

    if (shm_unlink(shm_name) != 0) perror("shm_unlink");

    char name[256];
    build_sem_name(name, sizeof(name), sem_prefix, "state");
    if (sem_unlink(name) != 0) perror("sem_unlink(state)");

    build_sem_name(name, sizeof(name), sem_prefix, "log");
    if (sem_unlink(name) != 0) perror("sem_unlink(log)");

    return 0;
}