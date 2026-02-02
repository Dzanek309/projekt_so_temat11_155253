set(COMMON_SOURCES
  ipc.cpp
  cli.cpp
  util.cpp
  logging.cpp
)

add_executable(tramwaj
  tramwaj.cpp
  ${COMMON_SOURCES}
)

add_executable(captain
  captain.cpp
  ${COMMON_SOURCES}
)

add_executable(passenger
  passenger.cpp
  ${COMMON_SOURCES}
)

add_executable(dispatcher
  dispatcher.cpp
  ${COMMON_SOURCES}
)
