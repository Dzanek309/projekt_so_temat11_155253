#ifndef COMMON_H
#define COMMON_H	

// Wspólne definicje dla wszystkich procesów (launcher/dispatcher/captain/passenger).

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======= Limity  =======
// limity dla kompliacji, potem do zmiany kiedy bêd¹ testy dla wiêkszych wartoœci
enum {MAX_K = 512};
enum {MAX_P = 1000};
enum {BRIDGE_Q_CAP = 1024};


// ======= Stany i kierunki  =======
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
	BRIDGE_DIR_IN = 1, // l¹d -> statek
	BRIDGE_DIR_OUT = 2 // statek -> l¹d
} bridge_dir_t;


// ======= Kolejka/deque na mostku =======
// W normalnym ruchu:
/// DIR_IN  : push_back (wejœcie na mostek od strony l¹du), pop_front (wejœcie na statek)
/// DIR_OUT : push_front (wejœcie na mostek od strony statku), pop_back (zejœcie na l¹d)
//
// Przy odp³ywaniu: kapitan wymusza DIR_OUT i usuwa z back (LIFO).

typedef struct {
	pid_t pid;
	uint8_t units; // 1 albo 2(rower)
	uint8_t evicting; // czy pasa¿er jest usuwany przez kapitana przy odp³ywaniu
} bridge_node_t;

// ======= Komunikaty (SysV msgqueue) =======
// Kapitan -> pasa¿er: mtype = PID pasa¿era
// Pasa¿er -> kapitan: mtype = 1

typedef enum {
	CMD_EVICT = 1
} cmd_t;

typedef struct {
	long mtype;          // PID adresata
	cmd_t cmd;
	int32_t trip_no;
} msg_cmd_t;

typedef struct {
	long mtype;          // 1
	pid_t pid;
	int32_t trip_no;
} msg_ack_t;

#ifdef __cplusplus
}
#endif

#endif // COMMON_H