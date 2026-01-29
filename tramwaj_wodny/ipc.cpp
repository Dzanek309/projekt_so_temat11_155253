#include "ipc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void build_sem_name(char* out, size_t out_sz, const char* prefix, const char* suffix) {
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

    // SHM
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    h->shm_fd = fd;

    if (ftruncate(fd, (off_t)sizeof(shm_state_t)) != 0) { perror("ftruncate"); return -1; }

    void* p = mmap(NULL, sizeof(shm_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }
    h->shm = (shm_state_t*)p;
    memcpy(h->shm, initial_state, sizeof(shm_state_t));

    // Semafory
    char name[256];

    build_sem_name(name, sizeof(name), sem_prefix, "state");
    h->sem_state = sem_open_create(name, 1);
    if (h->sem_state == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "log");
    h->sem_log = sem_open_create(name, 1);
    if (h->sem_log == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "seats");
    h->sem_seats = sem_open_create(name, (unsigned)initial_state->N);
    if (h->sem_seats == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "bikes");
    h->sem_bikes = sem_open_create(name, (unsigned)initial_state->M);
    if (h->sem_bikes == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "bridge");
    h->sem_bridge = sem_open_create(name, (unsigned)initial_state->K);
    if (h->sem_bridge == SEM_FAILED) return -1;

    // Message queue (SysV)
    int msqid = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600);
    if (msqid < 0) { perror("msgget"); return -1; }
    h->msqid = msqid;
    *out_msqid = msqid;

    return 0;
}

int ipc_open(ipc_handles_t* h, const char* shm_name, const char* sem_prefix, int msqid) {
    if (!h || !shm_name || !sem_prefix) return -1;
    memset(h, 0, sizeof(*h));
    snprintf(h->shm_name, sizeof(h->shm_name), "%s", shm_name);
    snprintf(h->sem_prefix, sizeof(h->sem_prefix), "%s", sem_prefix);

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

    build_sem_name(name, sizeof(name), sem_prefix, "seats");
    h->sem_seats = sem_open_existing(name);
    if (h->sem_seats == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "bikes");
    h->sem_bikes = sem_open_existing(name);
    if (h->sem_bikes == SEM_FAILED) return -1;

    build_sem_name(name, sizeof(name), sem_prefix, "bridge");
    h->sem_bridge = sem_open_existing(name);
    if (h->sem_bridge == SEM_FAILED) return -1;

    h->msqid = msqid;
    return 0;
}

void ipc_close(ipc_handles_t* h) {
    if (!h) return;
    if (h->shm && h->shm != MAP_FAILED) munmap(h->shm, sizeof(shm_state_t));
    h->shm = NULL;
    if (h->shm_fd > 0) close(h->shm_fd);
    h->shm_fd = -1;

    if (h->sem_state && h->sem_state != SEM_FAILED) sem_close(h->sem_state);
    if (h->sem_log && h->sem_log != SEM_FAILED) sem_close(h->sem_log);
    if (h->sem_seats && h->sem_seats != SEM_FAILED) sem_close(h->sem_seats);
    if (h->sem_bikes && h->sem_bikes != SEM_FAILED) sem_close(h->sem_bikes);
    if (h->sem_bridge && h->sem_bridge != SEM_FAILED) sem_close(h->sem_bridge);

    h->sem_state = h->sem_log = h->sem_seats = h->sem_bikes = h->sem_bridge = SEM_FAILED;
}

int ipc_destroy(const char* shm_name, const char* sem_prefix, int msqid) {
    if (!shm_name || !sem_prefix) return -1;

    // SHM unlink
    if (shm_unlink(shm_name) != 0) {
        // nie traktuj jako fatal
        perror("shm_unlink");
    }

    char name[256];
    build_sem_name(name, sizeof(name), sem_prefix, "state");
    if (sem_unlink(name) != 0) perror("sem_unlink(state)");

    build_sem_name(name, sizeof(name), sem_prefix, "log");
    if (sem_unlink(name) != 0) perror("sem_unlink(log)");

    build_sem_name(name, sizeof(name), sem_prefix, "seats");
    if (sem_unlink(name) != 0) perror("sem_unlink(seats)");

    build_sem_name(name, sizeof(name), sem_prefix, "bikes");
    if (sem_unlink(name) != 0) perror("sem_unlink(bikes)");

    build_sem_name(name, sizeof(name), sem_prefix, "bridge");
    if (sem_unlink(name) != 0) perror("sem_unlink(bridge)");

    // msg queue remove
    if (msqid >= 0) {
        if (msgctl(msqid, IPC_RMID, NULL) != 0) perror("msgctl(IPC_RMID)");
    }
    return 0;
}

// ======= Deque ops (ring buffer) =======
static int idx_next(int i) { return (i + 1) % BRIDGE_Q_CAP; }
static int idx_prev(int i) { return (i - 1 + BRIDGE_Q_CAP) % BRIDGE_Q_CAP; }

int bridge_is_empty(shm_state_t* s) {
    return s->bridge.count == 0;
}

bridge_node_t* bridge_front(shm_state_t* s) {
    if (s->bridge.count == 0) return NULL;
    return &s->bridge.q[s->bridge.head];
}

bridge_node_t* bridge_back(shm_state_t* s) {
    if (s->bridge.count == 0) return NULL;
    int last = idx_prev(s->bridge.tail);
    return &s->bridge.q[last];
}

int bridge_push_back(shm_state_t* s, bridge_node_t node) {
    if (s->bridge.count >= BRIDGE_Q_CAP - 1) return -1;
    s->bridge.q[s->bridge.tail] = node;
    s->bridge.tail = idx_next(s->bridge.tail);
    s->bridge.count++;
    s->bridge.load_units += node.units;
    return 0;
}

int bridge_push_front(shm_state_t* s, bridge_node_t node) {
    if (s->bridge.count >= BRIDGE_Q_CAP - 1) return -1;
    s->bridge.head = idx_prev(s->bridge.head);
    s->bridge.q[s->bridge.head] = node;
    s->bridge.count++;
    s->bridge.load_units += node.units;
    return 0;
}

int bridge_pop_front(shm_state_t* s, bridge_node_t* out) {
    if (s->bridge.count == 0) return -1;
    bridge_node_t n = s->bridge.q[s->bridge.head];
    s->bridge.head = idx_next(s->bridge.head);
    s->bridge.count--;
    s->bridge.load_units -= n.units;
    if (out) *out = n;
    return 0;
}

int bridge_pop_back(shm_state_t* s, bridge_node_t* out) {
    if (s->bridge.count == 0) return -1;
    int last = idx_prev(s->bridge.tail);
    bridge_node_t n = s->bridge.q[last];
    s->bridge.tail = last;
    s->bridge.count--;
    s->bridge.load_units -= n.units;
    if (out) *out = n;
    return 0;
}
