# Projekt - Tramwaj wodny (Temat 11)

Projekt polega na zasymulowaniu działania tramwaju wodnego kursującego latem na trasie **Kraków Wawel <-> Tyniec**. W symulacji biorą udział pasażerowie (część z rowerami), mostek, statek oraz dyspozytor wysyłający sygnały sterujące.

## Założenia systemu

- Statek może zabrać maksymalnie **N pasażerów** oraz **M rowerów** (M < N)
- Wejście na pokład odbywa się przez **mostek o pojemności K** (K < N)
- Pasażer z rowerem zajmuje na mostku **2 miejsca**
- Co **T1 jednostek czasu** statek wypływa automatycznie, chyba że wcześniej dyspozytor wyśle **sygnał1** wymuszający natychmiastowy rejs
- Podczas odpływania **mostek musi być całkowicie pusty** - pasażerowie, którzy nie weszli, schodzą w odwrotnej kolejności
- Rejs trwa **T2 jednostki czasu**
- Po dopłynięciu do Tyńca wszyscy pasażerowie opuszczają statek
- Mostek działa **jednokierunkowo** - w danym momencie pasażerowie mogą albo *wchodzić*, albo *schodzić*
- Statek może wykonać maksymalnie **R rejsów** w ciągu dnia
- Dyspozytor może wysłać **sygnał2**, który przerywa działanie systemu:
    - jeśli statek jest ładowany - nie wypływa i pasażerowie schodzą
    - jeśli statek płynie - kończy rejs normalnie

---

# Testy

## Test 1 - Dużo pasażerów, brak zakleszczeń

**Cel:** sprawdzić odporność systemu na duże obciążenie i brak deadlocków.

**Parametry:**
- N = 50, M = 20, K = 10  
- T1 = 20, T2 = 10  
- R = 2  
- Ok. 200 pasażerów pojawiających się losowo

**Oczekiwania:**
- brak deadlocków
- nieprzekroczone limity (pasażerowie, rowery, mostek)
- działanie kończy się poprawnie

---

## Test 2 - Zwolnienie zasobów IPC

**Cel:** upewnić się, że po zakończeniu programu zwalniane są wszystkie struktury IPC.

**Oczekiwania:**
- brak pozostałych semaforów, pamięci współdzielonej, kolejek komunikatów
- brak procesów zombie

---

## Test 3 - Ograniczenie liczby rejsów (R)

**Cel:** sprawdzić, czy system kończy działanie po wykonaniu maksymalnej liczby rejsów.

**Parametry:**
- N = 5, M = 5, K = 5  
- T1 = 5, T2 = 5  
- R = 3  

**Oczekiwania:**
- po wykonaniu R rejsów system kończy pracę
- logi wskazują liczba_wykonanych_rejsow == R
- nowi pasażerowie nie są wpuszczani po zakończeniu kursów

---

## Test 4 - Jednokierunkowy ruch na mostku

**Cel:** sprawdzić poprawne przełączanie kierunku ruchu na mostku.

**Przebieg:**
1. Rejs 1 - pasażerowie wchodzą na statek w Krakowie
2. W Tyńcu pasażerowie wychodzą przez mostek
3. Dopiero po opróżnieniu mostka nowi pasażerowie mogą wejść na rejs powrotny

**Oczekiwania:**
- nigdy nie ma jednocześnie osób wchodzących i schodzących
- zmienna np. kierunek_mostka = IN/OUT zmienia się tylko wtedy, gdy mostek jest pusty

---

# Repozytorium projektu

https://github.com/Dzanek309/projekt_so_temat11_155253
