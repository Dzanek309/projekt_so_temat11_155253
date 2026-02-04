# Projekt - Tramwaj wodny (Temat 11)

Projekt polega na zasymulowaniu działania tramwaju wodnego kursującego latem na trasie **Kraków Wawel <-> Tyniec**. W symulacji biorą udział pasażerowie (część z rowerami), mostek, statek oraz dyspozytor wysyłający sygnały sterujące.

## Założenia systemu

- Statek może zabrać maksymalnie **N pasażerów** oraz **M rowerów** (M < N)
- Wejście na pokład odbywa się przez **mostek o pojemności K** (K < N)
- Pasażer z rowerem zajmuje na mostku **2 miejsca**
- Co **T1 jednostek czasu** statek wypływa automatycznie, chyba że wcześniej dyspozytor wyśle **sygnał1** wymuszający natychmiastowy rejs
- Podczas odpływania **mostek musi być całkowicie pusty** — pasażerowie, którzy nie weszli, schodzą w odwrotnej kolejności
- Rejs trwa **T2 jednostki czasu**
- Po dopłynięciu do Tyńca wszyscy pasażerowie opuszczają statek
- Mostek działa **jednokierunkowo** — w danym momencie pasażerowie mogą albo *wchodzić*, albo *schodzić*
- Statek może wykonać maksymalnie **R rejsów** w ciągu dnia
- Dyspozytor może wysłać **sygnał2**, który przerywa działanie systemu:
  - jeśli statek jest ładowany — nie wypływa i pasażerowie schodzą
  - jeśli statek płynie — kończy rejs normalnie

---

# Testy

### Test 1 – podstawowy przebieg bez sygnałów z dużą ilością pasażerów  

**Przykład uruchomienia:**  
`./tramwaj --N 256 --M 25 --K 45 --T1 1000 --T2 1500 --R 3 --P 5000 --bike-prob 0.3 --log simulation.log`

**Oczekiwane:**
- wykonanie dokładnie **R rejsów**,
- po każdym `UNLOADING` zmienna `onboard_passengers` wraca do 0,
- brak zawieszeń,
- log zawiera podsumowania `TRIP SUMMARY`.

---

### Test 2 – mały mostek i dużo rowerów (sprawdzenie K i units=2)  

**Przykład uruchomienia:**  
`./tramwaj --N 20 --M 10 --K 3 --T1 1500 --T2 1000 --R 2 --P 40 --bike-prob 0.8 --log simulation.log`

**Oczekiwane:**
- brak przekroczeń pojemności mostka (K jednostek),
- pasażerowie z rowerem częściej rezygnują / wykonują rollback,
- system działa stabilnie.

---

### Test 3 – wcześniejszy odpływ (SIGUSR1)  

Uruchomić jak w Teście 1.  
Podczas fazy `LOADING` w dispatcherze wpisać `1` i nacisnąć Enter.

**Oczekiwane:**
- kapitan kończy `LOADING` przed upływem T1,
- przechodzi do `DEPARTING` i czyści mostek,
- dopiero potem przechodzi do `SAILING`.

---

### Test 4 – stop podczas LOADING (SIGUSR2)  

Uruchomić symulację.  
Podczas fazy `LOADING` w dispatcherze wpisać `2` i nacisnąć Enter.

**Oczekiwane:**
- statek nie rozpoczyna rejsu (brak `SAILING`),
- kapitan przechodzi do `UNLOADING` na miejscu i kończy działanie,
- faza końcowa `PHASE_END`,
- procesy kończą się poprawnie,
- zasoby IPC zostają sprzątnięte.
