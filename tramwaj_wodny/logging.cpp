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
	(void)lg; (void)path; (void)sem_log;
	return -1;
}

void logger_close(logger_t* lg) {
	(void)lg;
}

void logf(logger_t* lg, const char* role, const char* fmt, ...) {
	(void)lg; (void)role; (void)fmt;
}
