# These window-free diagnostics must run on Linux as well as macOS and Windows.
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/friends_retire.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/handoff_state.cmake")
foreach(case IN ITEMS friends_input session_logs net_diagnostics)
  add_executable(port_${case}_test "${CMAKE_CURRENT_LIST_DIR}/${case}_test.cpp")
  target_include_directories(port_${case}_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/host"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/include")
  target_compile_features(port_${case}_test PRIVATE cxx_std_17)
  add_test(NAME port_${case} COMMAND port_${case}_test)
endforeach()
if(NOT WIN32)
  enable_language(C)
  foreach(case IN ITEMS socket posix_ring)
    add_executable(port_yg_enet_${case}_test
      "${CMAKE_CURRENT_LIST_DIR}/yg_enet_${case}_test.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/yg_enet_socket.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/callbacks.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/compress.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/host.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/list.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/packet.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/peer.c"
      "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/protocol.c")
    if(case STREQUAL "posix_ring")
      target_sources(port_yg_enet_${case}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/unix.c")
    endif()
    target_include_directories(port_yg_enet_${case}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/include")
    # enet's POSIX platform switches, as the Linux full build gives full_enet (port/linux/full/CMakeLists.txt): without
    # them gcc-14 refuses unix.c's own socklen_t typedef.
    target_compile_definitions(port_yg_enet_${case}_test PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1
                               HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)
    target_compile_features(port_yg_enet_${case}_test PRIVATE c_std_11)
    find_package(Threads REQUIRED)
    target_link_libraries(port_yg_enet_${case}_test PRIVATE Threads::Threads)
    add_test(NAME port_yg_enet_${case} COMMAND port_yg_enet_${case}_test)
  endforeach()
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME port_friends_menu_wiring COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/friends_menu_wiring_test.py")

# Exercise the exact game-side label adapter without requiring a disc or the game toolchain.
get_filename_component(FRIENDS_LABEL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FRIENDS_LABEL_ADAPTER "${CMAKE_CURRENT_BINARY_DIR}/friends_label_adapter.inc")
add_custom_command(OUTPUT "${FRIENDS_LABEL_ADAPTER}"
  COMMAND "${Python3_EXECUTABLE}" "${FRIENDS_LABEL_ROOT}/tools/native_major_fixture.py"
    "${FRIENDS_LABEL_ROOT}/sourceport/game/shim/mu_menu_label.c" "${FRIENDS_LABEL_ADAPTER}"
    --function mu_slippi_friends_label
  DEPENDS "${FRIENDS_LABEL_ROOT}/tools/native_major_fixture.py"
    "${FRIENDS_LABEL_ROOT}/sourceport/game/shim/mu_menu_label.c"
  VERBATIM)
add_executable(port_friends_label_test "${CMAKE_CURRENT_LIST_DIR}/friends_label_test.cpp" "${FRIENDS_LABEL_ADAPTER}")
target_include_directories(port_friends_label_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
  "${FRIENDS_LABEL_ROOT}/sourceport/game/shim")
target_compile_features(port_friends_label_test PRIVATE cxx_std_17)
add_test(NAME port_friends_label COMMAND port_friends_label_test)

add_test(NAME port_online_menu COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/online_menu_test.py")
add_test(NAME port_slippi_title COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/slippi_title_test.py")

add_test(NAME port_css_state_wiring COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/css_state_wiring_test.py")
enable_language(C)
add_executable(port_css_state_test "${CMAKE_CURRENT_LIST_DIR}/css_state_test.c"
  "${FRIENDS_LABEL_ROOT}/sourceport/game/shim/mu_css_state.c")
target_include_directories(port_css_state_test PRIVATE "${FRIENDS_LABEL_ROOT}/sourceport/game/include"
  "${FRIENDS_LABEL_ROOT}/port/runtime/abi")
target_compile_definitions(port_css_state_test PRIVATE MU_NATIVE)
target_compile_features(port_css_state_test PRIVATE c_std_11)
add_test(NAME port_css_state COMMAND port_css_state_test)
