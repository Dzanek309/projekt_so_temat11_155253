#ifndef LOGGING_H
#define LOGGING_H

#include <semaphore.h>
#include <stdarg.h>

// Prosty logger do pliku (append). U¿ywa semafora do serializacji wpisów.

#ifdef __cplusplus
extern "C" {
#endif

	typedef struct {
		int fd;           // open()'owany plik
		sem_t* sem_log;   // named semaphore (binary)
	} logger_t;

	int logger_open(logger_t* lg, const char* path, sem_t* sem_log);
	void logger_close(logger_t* lg);

	// log line: [ms] pid role event details...
	void logf(logger_t* lg, const char* role, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // LOGGING_H
