# Shared by root's portable diagnostics and the Windows experimental port.
if(NOT TARGET port_handoff_state_test)
  find_package(Threads REQUIRED)
  add_executable(port_handoff_state_test "${CMAKE_CURRENT_LIST_DIR}/handoff_state_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/handoff_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/native_savestate.cpp")
  target_include_directories(port_handoff_state_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle")
  target_compile_features(port_handoff_state_test PRIVATE cxx_std_17)
  target_link_libraries(port_handoff_state_test PRIVATE Threads::Threads)
  add_test(NAME port_handoff_state COMMAND port_handoff_state_test "${CMAKE_CURRENT_BINARY_DIR}")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME port_handoff_diff COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_LIST_DIR}/handoff_diff_test.py" "${CMAKE_CURRENT_BINARY_DIR}")
  foreach(variant IN ITEMS hook off)
    add_executable(port_handoff_agent_${variant}_test "${CMAKE_CURRENT_LIST_DIR}/handoff_agent_test.cpp")
    target_include_directories(port_handoff_agent_${variant}_test PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle" "${CMAKE_CURRENT_LIST_DIR}/../third_party")
    target_compile_features(port_handoff_agent_${variant}_test PRIVATE cxx_std_17)
    target_link_libraries(port_handoff_agent_${variant}_test PRIVATE Threads::Threads)
    if(variant STREQUAL "hook")
      target_compile_definitions(port_handoff_agent_${variant}_test PRIVATE YOUGAME_AGENT_TEST_HOOK)
    endif()
    add_test(NAME port_handoff_agent_${variant} COMMAND port_handoff_agent_${variant}_test "${CMAKE_CURRENT_BINARY_DIR}")
  endforeach()
endif()
if(NOT TARGET port_handoff_delta_test)
  add_executable(port_handoff_delta_test "${CMAKE_CURRENT_LIST_DIR}/handoff_delta_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/handoff_state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/native_savestate.cpp")
  target_include_directories(port_handoff_delta_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle")
  target_compile_features(port_handoff_delta_test PRIVATE cxx_std_17)
  target_link_libraries(port_handoff_delta_test PRIVATE Threads::Threads)
  add_test(NAME port_handoff_delta COMMAND port_handoff_delta_test)
  add_test(NAME port_handoff_quiescence COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_LIST_DIR}/handoff_quiescence_test.py")
endif()
if(NOT TARGET port_friends_handoff_join_test)
  add_executable(port_friends_handoff_join_test "${CMAKE_CURRENT_LIST_DIR}/yougame_console_link_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/yougame_console_link.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle/yougame_console_engine.cpp")
  target_include_directories(port_friends_handoff_join_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle" "${CMAKE_CURRENT_LIST_DIR}/../third_party")
  target_compile_features(port_friends_handoff_join_test PRIVATE cxx_std_17)
  target_compile_definitions(port_friends_handoff_join_test PRIVATE YOUGAME_AGENT_TEST_HOOK)
  target_link_libraries(port_friends_handoff_join_test PRIVATE Threads::Threads)
  add_test(NAME port_friends_handoff_join COMMAND port_friends_handoff_join_test --handoff-join)
endif()
