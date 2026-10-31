# Synthetic relay output and present-policy checks, also available before the Linux return.
foreach(variant hook off)
  add_executable(port_yougame_relay_output_${variant}_test
    "${PROJECT_SOURCE_DIR}/port/tests/yougame_relay_output_test.cpp")
  target_include_directories(port_yougame_relay_output_${variant}_test PRIVATE
    "${PROJECT_SOURCE_DIR}/port/runtime/hle" "${PROJECT_SOURCE_DIR}/port/runtime/host"
    "${PROJECT_SOURCE_DIR}/port/runtime/ppc" "${PROJECT_SOURCE_DIR}/port/third_party")
  if(NOT WIN32)
    target_include_directories(port_yougame_relay_output_${variant}_test PRIVATE "${PROJECT_SOURCE_DIR}/port/linux/compat")
  endif()
  target_compile_features(port_yougame_relay_output_${variant}_test PRIVATE cxx_std_17)
  target_compile_definitions(port_yougame_relay_output_${variant}_test PRIVATE NOMINMAX)
  if(variant STREQUAL "hook")
    target_compile_definitions(port_yougame_relay_output_${variant}_test PRIVATE YOUGAME_AGENT_TEST_HOOK)
  endif()
  if(APPLE)
    set_target_properties(port_yougame_relay_output_${variant}_test PROPERTIES OSX_ARCHITECTURES x86_64)
  endif()
  add_test(NAME port_yougame_relay_output_${variant} COMMAND port_yougame_relay_output_${variant}_test)
endforeach()
add_test(NAME port_relay_output_wiring COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/port/tests/relay_output_wiring_test.py")
add_test(NAME port_yougame_relay_output_absent COMMAND sh
  "${PROJECT_SOURCE_DIR}/port/tests/agent_test_hook_absent.sh"
  $<TARGET_FILE:port_yougame_relay_output_off_test>)
foreach(case timing rumble family present pulse)
  add_test(NAME port_yougame_relay_${case} COMMAND port_yougame_relay_output_hook_test ${case})
endforeach()
