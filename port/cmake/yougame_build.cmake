# The full YouGame build this tree makes (e.g. 0.9.120-online-test.103), for the first line of the
# game's log (runtime/host/startup_clock.h). One line in YOUGAME_BUILD at the root; bump it with each
# build. -DMELEE_YOUGAME_BUILD=<label> overrides it, and an empty value logs no build.
if(NOT DEFINED MELEE_YOUGAME_BUILD)
  set(MELEE_YOUGAME_BUILD "")
  if(EXISTS "${PROJECT_SOURCE_DIR}/YOUGAME_BUILD")
    file(STRINGS "${PROJECT_SOURCE_DIR}/YOUGAME_BUILD" MELEE_YOUGAME_BUILD LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/YOUGAME_BUILD")
  endif()
endif()
string(STRIP "${MELEE_YOUGAME_BUILD}" MELEE_YOUGAME_BUILD)
# A compile-time string literal that lands in a log line: keep it short and plain.
if(NOT "${MELEE_YOUGAME_BUILD}" MATCHES "^[A-Za-z0-9._+-]*$")
  message(FATAL_ERROR "YOUGAME_BUILD must be letters, digits, '.', '_', '+' or '-' (got '${MELEE_YOUGAME_BUILD}')")
endif()
string(LENGTH "${MELEE_YOUGAME_BUILD}" MELEE_YOUGAME_BUILD_LENGTH)
if(MELEE_YOUGAME_BUILD_LENGTH GREATER 64)
  message(FATAL_ERROR "YOUGAME_BUILD is longer than 64 characters")
endif()
