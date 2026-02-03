#ifndef COMMON_H
#define COMMON_H

// Wspólne definicje dla wszystkich procesów (launcher/dispatcher/captain/passenger).
// Uwaga: struktury musz¹ byæ POD (Plain Old Data), bo s¹ mapowane przez SHM.

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

	// ======= Limity kompilacyjne =======
	// MAX_K / MAX_P to "bezpieczniki" na rozmiar SHM i liczbê procesów.
	enum { MAX_K = 512 };
	enum { MAX_P = 5000 };
	enum { BRIDGE_Q_CAP = 1024 }; // >= MAX_K (z zapasem)

	// ======= Stany i kierunki =======
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
		BRIDGE_DIR_NONE = 0,
		BRIDGE_DIR_IN = 1,   // l¹d -> statek
		BRIDGE_DIR_OUT = 2   // statek -> l¹d
	} bridge_dir_t;

	// ======= Kolejka/deque na mostku =======
	// W normalnym ruchu:
	/// DIR_IN  : push_back (wejœcie na mostek od strony l¹du), pop_front (wejœcie na statek)
	/// DIR_OUT : push_front (wejœcie na mostek od strony statku), pop_back (zejœcie na l¹d)
	//
	// Przy odp³ywaniu: kapitan wymusza DIR_OUT i usuwa z back (LIFO).

	typedef struct {
		pid_t pid;
		uint8_t units;      // 1 albo 2 (rower)
		uint8_t evicting;   // 1 jeœli kapitan nakaza³ zejœcie
	} bridge_node_t;

	typedef struct {
		bridge_dir_t dir;     // jednokierunkowoœæ
		int32_t load_units;   // zajête jednostki (pomocniczo)
		int32_t count;        // ilu fizycznie na mostku (wpisów)
		int32_t head;         // indeks head w ring buffer
		int32_t tail;         // indeks tail w ring buffer (pierwszy wolny)
		bridge_node_t q[BRIDGE_Q_CAP];
	} bridge_state_t;

	typedef struct {
		// Konfiguracja (ustawiana przez launcher)
		int32_t N, M, K;
		int32_t T1_ms, T2_ms;
		int32_t R;
		int32_t P;

		// Stan globalny
		phase_t phase;
		dir_t direction;
		int32_t boarding_open;        // 1 w LOADING, 0 w DEPARTING/...
		int32_t trip_no;              // numer aktualnego rejsu (1..)
		int32_t shutdown;             // ustawiane przez launcher przy SIGINT/SIGTERM

		// Liczniki (aktualizowane przez pasa¿erów/kapitana pod mutexem)
		int32_t onboard_passengers;
		int32_t onboard_bikes;

		// Mostek
		bridge_state_t bridge;

		// PID kapitana (dla wygody/debug)
		pid_t captain_pid;
	} shm_state_t;

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
