#include "logging.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int logger_open(logger_t* lg, const char* path, sem_t* sem_log) {
    if (!lg || !path || !sem_log) return -1;

    lg->sem_log = sem_log;

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
    (void)lg; (void)role; (void)fmt;
}
