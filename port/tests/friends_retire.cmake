# Shared registration: root diagnostics (including Linux) and the experimental port.
if(NOT TARGET port_friends_retire_test)
  find_package(Threads REQUIRED)
  add_executable(port_friends_retire_test "${CMAKE_CURRENT_LIST_DIR}/friends_retire_test.cpp")
  target_include_directories(port_friends_retire_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/hle" "${CMAKE_CURRENT_LIST_DIR}/../third_party")
  target_compile_features(port_friends_retire_test PRIVATE cxx_std_17)
  target_link_libraries(port_friends_retire_test PRIVATE Threads::Threads)
  foreach(case IN ITEMS slow sequence exit)
    add_test(NAME port_friends_retire_${case} COMMAND port_friends_retire_test ${case})
  endforeach()
endif()
