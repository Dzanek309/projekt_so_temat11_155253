# Projekt SO – Temat 11: Tramwaj wodny

Repozytorium:
https://github.com/Dzanek309/projekt_so_temat11_155253/tree/main/tramwaj_wodny

## 1. Cel projektu
Celem projektu jest symulacja „tramwaju wodnego” kursującego na trasie **Kraków ↔ Tyniec** z ograniczeniami:
- statek ma pojemność **N** pasażerów,
- może przewieźć **M** rowerów (M < N),
- mostek ma pojemność **K** „jednostek”, gdzie pasażer z rowerem zajmuje **2** jednostki,
- statek odpływa co **T1** lub wcześniej po poleceniu dyspozytora (**sygnał1**),
- rejs trwa **T2**,
- w momencie odpływania kapitan dopilnowuje, aby **mostek był pusty**; osoby, które nie weszły na statek schodzą z mostka **od końca kolejki (LIFO)**,
- po dopłynięciu pasażerowie opuszczają statek; ruch na mostku jest **jednokierunkowy**,
- statek wykonuje maksymalnie **R** rejsów lub kończy po poleceniu dyspozytora (**sygnał2**) zgodnie z opisem zadania.

Przebieg symulacji jest logowany do pliku tekstowego.

---

## 2. Założenia projektowe
1. **Procesy**: każda rola to osobny proces uruchamiany przez launcher (`fork()` + `execv()`).
2. **Wspólny stan** symulacji jest utrzymywany w **pamięci dzielonej POSIX** (SHM) i chroniony semaforem-mutexem.
3. **Mechanizmy IPC**:
   - pamięć dzielona POSIX (`shm_open`, `mmap`) do trzymania stanu,
   - semafory POSIX nazwane (`sem_open`, `sem_wait`, `sem_post`) do synchronizacji i limitów,
   - kolejka komunikatów SysV (`msgget`, `msgsnd`, `msgrcv`) do poleceń ewakuacji (CMD_EVICT) i potwierdzeń (ACK).
   - łącze nienazwane `pipe()` – launcher tworzy potok do komunikacji z procesem guardian (sprzątanie IPC przy śmierci launchera).
4. **Minimalne prawa dostępu**:
   - `umask(0077)` w launcherze,
   - SHM, semafory, kolejka komunikatów i log z uprawnieniami `0600`.

---

## 3. Architektura i opis działania

### 3.1 Procesy / role
**Launcher (`tramwaj`)**
- parsuje i waliduje parametry (N, M, K, T1, T2, R, P, bike-prob, log),
- tworzy zasoby IPC (SHM + semafory + kolejka komunikatów),
- tworzy proces guardian (fork): czyta z potoku `pipe()`; przy normalnym zakończeniu launcher zapisuje bajt do potoku i guardian się kończy; przy śmierci launchera guardian wywołuje `ipc_destroy()` i zabija grupę (SIGTERM/SIGKILL),
- przed otwarciem logu wywołuje `unlink(log_path)` (nowy plik na sesję),
- uruchamia procesy: `captain`, `dispatcher`, `passenger` (wielokrotnie),
- obsługuje shutdown po SIGINT/SIGTERM: kończy dzieci (SIGTERM → SIGKILL), sprząta IPC, zapisuje bajt do potoku guardian.

**Kapitan (`captain`)**
- zarządza fazami rejsu: `LOADING → DEPARTING → SAILING → UNLOADING`,
- odpływa po T1 albo wcześniej po `SIGUSR1`,
- reaguje na `SIGUSR2`:
  - jeśli podczas LOADING: nie wypływa, przechodzi do UNLOADING i kończy,
  - jeśli podczas SAILING: kończy bieżący rejs normalnie i dopiero kończy,
- przed odpłynięciem opróżnia mostek w kolejności LIFO (od końca kolejki).

**Dyspozytor (`dispatcher`)**
- w trybie interaktywnym czyta komendy ze stdin:
  - `1` → wysyła `SIGUSR1` (wcześniejszy odpływ),
  - `2` → wysyła `SIGUSR2` (stop),
- w trybie IPC obserwuje flagi `shutdown` / `PHASE_END` w SHM i kończy pracę po ich wykryciu.

**Pasażer (`passenger`)**
- ma kierunek i opcjonalny rower (rower = 2 jednostki mostka),
- próbuje wejść na statek w czasie LOADING w swoim kierunku:
  - rezerwuje miejsce (N), rower (M) oraz jednostki mostka (K),
  - wchodzi na mostek i czeka w kolejce na wejście na statek,
- po dopłynięciu schodzi ze statku w fazie UNLOADING,
- obsługuje polecenie ewakuacji CMD_EVICT od kapitana (zdejście LIFO + wysłanie ACK).

---

## 4. Synchronizacja i komunikacja (IPC)

### 4.1 Pamięć dzielona (POSIX SHM) – stan symulacji
Struktura `shm_state_t` przechowuje m.in.:
- parametry (N, M, K, T1, T2, R, P),
- fazę rejsu i kierunek,
- flagi `boarding_open`, `shutdown`,
- liczniki `onboard_passengers`, `onboard_bikes`,
- stan mostka (deque w ring bufferze).

Dostęp do SHM jest chroniony semaforem `sem_state` (mutex dla procesów).

### 4.2 Semafory POSIX (named)
- `sem_state` – mutex do SHM,
- `sem_log` – mutex do logowania (żeby wpisy się nie mieszały),
- `sem_seats` – limit N miejsc na statku,
- `sem_bikes` – limit M rowerów,
- `sem_bridge` – limit K jednostek mostka.

### 4.3 Kolejka komunikatów SysV – ewakuacja mostka (LIFO)
- Kapitan wysyła do konkretnego pasażera komunikat `CMD_EVICT` na `mtype=PID`,
- Pasażer schodzi z mostka w kolejności LIFO i wysyła `ACK` na `mtype=1`,
- Kapitan czeka na ACK i przechodzi do kolejnego pasażera.

### 4.4 Sygnały
- `SIGUSR1` – wcześniejszy odpływ (dyspozytor → kapitan),
- `SIGUSR2` – stop rejsów (dyspozytor → kapitan),
- `SIGINT/SIGTERM/SIGHUP` – zakończenie (obsługa w procesach; launcher dodatkowo uruchamia procedurę shutdown).

### 4.5 Łącze nienazwane (pipe)
Launcher tworzy `guard_pipe` (`pipe()`), ustawia `FD_CLOEXEC` na obu końcach. Proces guardian w potomku: `read(guard_pipe[0])` – jeśli dostanie bajt od launchera, kończy się `_exit(0)`; w przeciwnym razie (launcher nie żyje) wywołuje `ipc_destroy()` i wysyła SIGTERM/SIGKILL do grupy. Launcher na koniec: `write(guard_pipe[1], ...)`, `close(guard_pipe[1])`.

---

## 5. Walidacja danych wejściowych i obsługa błędów
- Parametry launchera są sprawdzane (zakresy N/M/K/T1/T2/R/P, `bike-prob ∈ [0..1]`, niepusta ścieżka logu).
- Dodatkowo launcher sprawdza `RLIMIT_NPROC`, aby nie próbować uruchomić więcej procesów niż pozwala system.
- W projekcie zastosowano obsługę błędów syscalli z użyciem `perror()` oraz `errno`.
- Wprowadzono własną funkcję `die_perror()` (perror + exit) do krytycznych błędów.

---

## 6. Co udało się zrobić (zgodność z opisem zadania)
- Ograniczenia N/M/K oraz obsługa „rower zajmuje 2 miejsca na mostku”.
- Cykle rejsów z fazami LOADING/DEPARTING/SAILING/UNLOADING.
- Odpływ po T1 lub wcześniej po `SIGUSR1`.
- Stop po `SIGUSR2` zgodnie z opisem (różne zachowanie zależnie od fazy).
- Opróżnianie mostka przed odpłynięciem w kolejności LIFO.
- Jednokierunkowy ruch na mostku (IN/OUT).
- Logowanie przebiegu symulacji do pliku tekstowego (z synchronizacją wpisów).

---

## 7. Zauważone problemy / trudności
- Najbardziej wymagające było zapewnienie poprawnego opróżniania mostka w kolejności LIFO przy jednoczesnej asynchroniczności procesów pasażerów (rozwiązane przez deque w SHM + CMD_EVICT/ACK przez kolejkę SysV).
- Duża liczba ścieżek rollback/cleanup w procesie pasażera (rezerwacje miejsc/rowerów/mostka) – rozwiązane przez lokalne flagi stanu i „best-effort cleanup” przy kończeniu procesu.

---

## 8. Testy (min. 4) – opis i oczekiwane wyniki
> Testy uruchamiano wielokrotnie (losowość pasażerów). Weryfikacja odbywa się przez analizę logów: brak zakleszczeń i poprawne zakończenie procesów, brak przekroczeń limitów N/M/K, poprawna reakcja na sygnały.

### Test 1 – podstawowy przebieg bez sygnałów z dużą ilością pasażerów
Przykład:
`./tramwaj --N 256 --M 25 --K 45 --T1 1000 --T2 1500 --R 3 --P 5000 --bike-prob 0.3 --log simulation.log`

Oczekiwane:
- wykonanie dokładnie R rejsów,
- po każdym UNLOADING `onboard_passengers` wraca do 0,
- brak zawieszeń, log zawiera podsumowania TRIP SUMMARY.

### Test 2 – mały mostek i dużo rowerów (sprawdzenie K i units=2)
Przykład:
`./tramwaj --N 20 --M 10 --K 3 --T1 1500 --T2 1000 --R 2 --P 40 --bike-prob 0.8 --log simulation.log`

Oczekiwane:
- brak przekroczeń pojemności mostka (K jednostek),
- pasażerowie z rowerem częściej rezygnują/rollbackują, ale system działa stabilnie.

### Test 3 – wcześniejszy odpływ (SIGUSR1)
Uruchomić jak Test 1, podczas LOADING w dispatcherze wpisać `1` i Enter.

Oczekiwane:
- kapitan kończy LOADING przed upływem T1,
- przechodzi do DEPARTING i czyści mostek,
- dopiero potem przechodzi do SAILING.

### Test 4 – stop podczas LOADING (SIGUSR2)
Uruchomić symulację, podczas LOADING w dispatcherze wpisać `2` i Enter.

Oczekiwane:
- statek nie rozpoczyna rejsu (brak SAILING),
- kapitan przechodzi do UNLOADING na miejscu i kończy,
- faza końcowa `PHASE_END`, procesy kończą się, zasoby IPC sprzątnięte.

---

## 9. Instrukcja uruchomienia

### 9.1 Opis argumentów uruchomieniowych

Program uruchamia się jako:

`./tramwaj --N <int> --M <int> --K <int> --T1 <ms> --T2 <ms> --R <int> --P <int> [--bike-prob <0..1>] [--log <path>]`

#### Argumenty obowiązkowe
- `--N <int>` – **pojemność statku**: maksymalna liczba pasażerów, którzy mogą znajdować się na pokładzie jednocześnie.  
  W programie kontrolowane semaforem `sem_seats` ustawionym na N.

- `--M <int>` – **maksymalna liczba rowerów** na pokładzie (M < N).  
  Kontrolowane semaforem `sem_bikes` ustawionym na M.

- `--K <int>` – **pojemność mostka w jednostkach** (K < N).  
  Pasażer bez roweru zajmuje 1 jednostkę, pasażer z rowerem zajmuje 2 jednostki.  
  Kontrolowane semaforem `sem_bridge` ustawionym na K.

- `--T1 <ms>` – **czas załadunku (boarding)** w milisekundach.  
  Po upływie T1 kapitan kończy fazę LOADING i rozpoczyna procedurę odpływu (o ile nie otrzyma wcześniej SIGUSR1).

- `--T2 <ms>` – **czas rejsu** w milisekundach (czas fazy SAILING).

- `--R <int>` – **maksymalna liczba rejsów** do wykonania w symulacji.  
  Po wykonaniu R rejsów kapitan kończy działanie (przechodzi do PHASE_END).

- `--P <int>` – **liczba procesów pasażerów** tworzonych przez launcher (maks. limit symulacji).  
  Każdy pasażer to osobny proces uruchamiany przez `fork()` + `execv()`.

#### Argumenty opcjonalne
- `--bike-prob <0..1>` – **prawdopodobieństwo**, że losowo tworzony pasażer ma rower.  
  `0.0` oznacza brak rowerów, `1.0` oznacza że każdy pasażer ma rower.  
  Domyślnie: `0.0`.

- `--log <path>` – **ścieżka do pliku logów**, do którego zapisują wszystkie procesy (launcher/kapitan/dyspozytor/pasażerowie).  
  Domyślnie: `simulation.log`.

#### Przykład
`./tramwaj --N 20 --M 5 --K 6 --T1 1000 --T2 1500 --R 8 --P 60 --bike-prob 0.3 --log simulation.log`

---

## 10. Linki do istotnych fragmentów kodu (wstaw sam permalinki z GitHub)

### 10.1 Tworzenie i obsługa plików: `open(), close(), read(), write(), unlink()`
- logger: `open()/write()/close()`  
  Plik: `tramwaj_wodny/logging.cpp` (`logger_open()`, `logf()`, `logger_close()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/logging.cpp#L23-L82
- launcher: `unlink(log_path)` przed otwarciem logu  
  Plik: `tramwaj_wodny/tramwaj.cpp`  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L145-L147
- dyspozytor: `read()` ze stdin  
  Plik: `tramwaj_wodny/dispatcher.cpp` (pętla z `read(STDIN_FILENO, ...)`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/dispatcher.cpp#L203-L207

### 10.2 Tworzenie i obsługa procesów: `fork(), execv(), exit(), waitpid()`
- `fork()` + `execv()`  
  Plik: `tramwaj_wodny/tramwaj.cpp` (`spawn_exec()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L32-L40
- `waitpid()`  
  Plik: `tramwaj_wodny/tramwaj.cpp` (pętla oczekiwania na zakończenie dzieci)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L259-L267
- `exit()` (funkcja do krytycznych błędów)  
  Plik: `tramwaj_wodny/util.cpp` (`die_perror()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/util.cpp#L9-L12
- `_exit()` (guardian w tramwaj.cpp, błąd parsowania w dispatcher.cpp)  
  Plik: `tramwaj_wodny/tramwaj.cpp`, `tramwaj_wodny/dispatcher.cpp`  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L128-L141

### 10.3 Obsługa sygnałów: `sigaction(), kill()`
- `sigaction()` (instalacja handlerów)  
  Plik: `tramwaj_wodny/captain.cpp` (`install_handlers()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/captain.cpp#L22-L36
- `kill()` (wysyłanie SIGUSR1/SIGUSR2)  
  Plik: `tramwaj_wodny/dispatcher.cpp` (obsługa komend `1` i `2`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/dispatcher.cpp#L213-L229
- `kill()` (shutdown dzieci)  
  Plik: `tramwaj_wodny/tramwaj.cpp` (SIGTERM/SIGKILL podczas shutdown)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L231-L257

### 10.4 Synchronizacja procesów: `sem_open(), sem_wait(), sem_trywait(), sem_post(), sem_unlink()`
- tworzenie/otwieranie semaforów  
  Plik: `tramwaj_wodny/ipc.cpp` (`ipc_create()`, `ipc_open()`, `ipc_destroy()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/ipc.cpp#L37-L171
- użycie semaforów (mutex do SHM + limity)  
  Plik: `tramwaj_wodny/passenger.cpp` i `tramwaj_wodny/captain.cpp`  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/captain.cpp#L39-L46

### 10.5 Pamięć dzielona (POSIX): `shm_open(), ftruncate(), mmap(), munmap(), shm_unlink()`
- tworzenie/otwieranie/sprzątanie SHM  
  Plik: `tramwaj_wodny/ipc.cpp` (`ipc_create()`, `ipc_open()`, `ipc_destroy()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/ipc.cpp#L44-L53

### 10.6 Kolejki komunikatów (SysV): `msgget(), msgsnd(), msgrcv(), msgctl()`
- `msgget()` + `msgctl(IPC_RMID)`  
  Plik: `tramwaj_wodny/ipc.cpp` (`ipc_create()`, `ipc_destroy()`)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/ipc.cpp#L76-L80
- `msgsnd()`/`msgrcv()` (CMD_EVICT + ACK)  
  Plik: `tramwaj_wodny/captain.cpp` i `tramwaj_wodny/passenger.cpp`  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/captain.cpp#L84-L110

### 10.7 Łącze nienazwane: `pipe(), read(), write(), close()`
- `pipe(guard_pipe)`, `read(guard_pipe[0])`, `write(guard_pipe[1])`, `close()`  
  Plik: `tramwaj_wodny/tramwaj.cpp` (proces guardian i launcher)  
  Link: https://github.com/Dzanek309/projekt_so_temat11_155253/blob/3140e3ca0d3bea786b68e8c71e91675c83d8b1fb/tramwaj_wodny/tramwaj.cpp#L122-L145
