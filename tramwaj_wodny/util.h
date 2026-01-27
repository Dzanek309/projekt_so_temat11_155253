#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

	// B³¹d syscall -> perror + exit(1)
	void die_perror(const char* what);

	// Timestamp ms (monotoniczny)
	int64_t now_ms_monotonic(void);

	// Bezpieczne parsowanie liczby ca³kowitej
	// zwraca 0 ok, -1 b³¹d
	int parse_i32(const char* s, int32_t* out);

	// Bezpieczne parsowanie double
	int parse_double(const char* s, double* out);

	// Sleep ms (nanosleep)
	void sleep_ms(int ms);

#ifdef __cplusplus
}
#endif

#endif // UTIL_H
