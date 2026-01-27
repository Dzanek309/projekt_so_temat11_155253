#ifndef COMMON_H
#define COMMON_H	

// Wspólne definicje dla wszystkich procesów (launcher/dispatcher/captain/passenger).

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// limity dla kompliacji, potem do zmiany kiedy bêd¹ testy dla wiêkszych wartoœci
enum {MAX_K = 512};
enum {MAX_P = 1000};
enum {BRIDGE_Q_CAP = 1024};

typedef enum {
	PHASE_LOADING = 0,
	PHASE_DEPARTING = 1,
	PHASE_SAILING = 2,
	PHASE_UNLOADING = 3,
	PHASE_END = 4
} phase_t;

typedef enum {
	DIR_KRAKOW_TO_TYNIEC = 0,
	DIR_TYNIEC_TO_KRAKOW = 1
} dir_t;

typedef enum {
	BRIDGE_DIR_NONE= 0,
	BRIDGE_DIR_IN = 1,
	BRIDGE_DIR_OUT = 2
} bridge_dir_t;



#ifdef __cplusplus
}
#endif

#endif // COMMON_H