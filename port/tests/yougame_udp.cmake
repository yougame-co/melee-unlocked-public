# Keep both targets above the root Linux return(). Fake I/O never reaches a server.
find_package(Threads REQUIRED)
add_executable(port_yougame_udp_test "${CMAKE_CURRENT_LIST_DIR}/yougame_udp_test.cpp")
target_include_directories(port_yougame_udp_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle")
target_compile_features(port_yougame_udp_test PRIVATE cxx_std_17)
target_link_libraries(port_yougame_udp_test PRIVATE Threads::Threads)
add_test(NAME port_yougame_udp COMMAND port_yougame_udp_test)
if(NOT WIN32)
  enable_language(C)
  file(GLOB UDP_ENET_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/*.c")
  list(REMOVE_ITEM UDP_ENET_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/unix.c"
                                  "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/win32.c")
  add_executable(port_yougame_udp_enet_test "${CMAKE_CURRENT_LIST_DIR}/yougame_udp_enet_test.cpp" ${UDP_ENET_SOURCES})
  target_include_directories(port_yougame_udp_enet_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle"
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/enet/include")
  target_compile_features(port_yougame_udp_enet_test PRIVATE cxx_std_17)
  target_link_libraries(port_yougame_udp_enet_test PRIVATE Threads::Threads)
  add_test(NAME port_yougame_udp_enet COMMAND port_yougame_udp_enet_test)
endif()
