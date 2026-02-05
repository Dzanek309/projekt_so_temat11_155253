#include "logging.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static int sem_wait_nointr(sem_t* s) {
    while (sem_wait(s) != 0) {
        if (errno == EINTR) return -1;
        die_perror("sem_wait");
    }
    return 0;
}

static void sem_post_chk(sem_t* s) {
    if (sem_post(s) != 0) die_perror("sem_post");
}

int logger_open(logger_t* lg, const char* path, sem_t* sem_log) {
    if (!lg || !path || !sem_log) return -1;
    lg->sem_log = sem_log;
    // dzieci otwieraj? sw?j FD niezale?nie
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
        perror("open(log)");
        return -1;
    }
    lg->fd = fd;
    return 0;
}

void logger_close(logger_t* lg) {
    if (!lg) return;
    if (lg->fd >= 0) close(lg->fd);
    lg->fd = -1;
}

void logf(logger_t* lg, const char* role, const char* fmt, ...) {
    if (!lg || lg->fd < 0 || !role || !fmt) return;

    if (sem_wait_nointr(lg->sem_log) != 0) return;

    char buf[1024];
    int64_t ms = now_ms_monotonic();
    pid_t pid = getpid();

    int off = snprintf(buf, sizeof(buf), "[%lld] pid=%d role=%s ",
        (long long)ms, (int)pid, role);
    if (off < 0) off = 0;
    if (off >= (int)sizeof(buf)) off = (int)sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    (void)n;

    size_t len = strnlen(buf, sizeof(buf));
    // zapewnij newline
    if (len == 0 || buf[len - 1] != '\n') {
        if (len + 1 < sizeof(buf)) {
            buf[len] = '\n';
            buf[len + 1] = '\0';
            len++;
        }
    }

    size_t written = 0;
    while (written < len) {
        ssize_t wr = write(lg->fd, buf + written, len - written);
        if (wr < 0) {
            perror("write(log)");
            break;
        }
        written += (size_t)wr;
    }

    sem_post_chk(lg->sem_log);
}
