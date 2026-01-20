# CMakeList.txt: projekt CMake dla elementu tramwaj_wodny, dołącz źródło i zdefiniuj
# tutaj logikę specyficzną dla projektu.
#

# Dodaj źródło do pliku wykonywalnego tego projektu.
add_executable (tramwaj_wodny "tramwaj_wodny.cpp" "tramwaj_wodny.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET tramwaj_wodny PROPERTY CXX_STANDARD 20)
endif()

# TODO: Dodaj testy i zainstaluj elementy docelowe w razie potrzeby.
