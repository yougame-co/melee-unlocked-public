# Controller profiles and overlay binding access: no window, device or game process.
add_executable(port_controller_profiles_test
  "${PROJECT_SOURCE_DIR}/port/tests/controller_profiles_test.cpp"
  "${PROJECT_SOURCE_DIR}/port/runtime/host/controller_profiles.cpp")
target_include_directories(port_controller_profiles_test PRIVATE
  "${PROJECT_SOURCE_DIR}/port/runtime/host")
target_compile_features(port_controller_profiles_test PRIVATE cxx_std_17)
add_test(NAME port_controller_profiles COMMAND port_controller_profiles_test)

add_executable(port_settings_profile_bindings_test
  "${PROJECT_SOURCE_DIR}/port/tests/settings_profile_bindings_test.cpp")
target_include_directories(port_settings_profile_bindings_test PRIVATE
  "${PROJECT_SOURCE_DIR}/port/runtime/gx")
target_compile_features(port_settings_profile_bindings_test PRIVATE cxx_std_17)
add_test(NAME port_settings_profile_bindings COMMAND port_settings_profile_bindings_test)

add_executable(port_dolphin_profile_test
  "${PROJECT_SOURCE_DIR}/port/tests/dolphin_profile_test.cpp"
  "${PROJECT_SOURCE_DIR}/port/runtime/host/dolphin_profile.cpp")
target_include_directories(port_dolphin_profile_test PRIVATE
  "${PROJECT_SOURCE_DIR}/port/runtime/host")
target_compile_features(port_dolphin_profile_test PRIVATE cxx_std_17)
if(WIN32)
  target_compile_definitions(port_dolphin_profile_test PRIVATE NOMINMAX)
  target_link_libraries(port_dolphin_profile_test PRIVATE comdlg32 user32)
endif()
add_test(NAME port_dolphin_profile COMMAND port_dolphin_profile_test)

# Window-free relay family and routing checks (Windows registers these in port/CMakeLists.txt).
if(NOT WIN32)
  foreach(relay_test controller_relay yougame_controller_relay)
    add_executable(port_${relay_test}_test "${PROJECT_SOURCE_DIR}/port/tests/${relay_test}_test.cpp")
    target_include_directories(port_${relay_test}_test PRIVATE
      "${PROJECT_SOURCE_DIR}/port/runtime/hle" "${PROJECT_SOURCE_DIR}/port/runtime/host"
      "${PROJECT_SOURCE_DIR}/port/runtime/ppc" "${PROJECT_SOURCE_DIR}/port/third_party"
      "${PROJECT_SOURCE_DIR}/port/linux/compat")
    target_compile_features(port_${relay_test}_test PRIVATE cxx_std_17)
    target_compile_definitions(port_${relay_test}_test PRIVATE NOMINMAX)
    if(APPLE)
      set_target_properties(port_${relay_test}_test PROPERTIES OSX_ARCHITECTURES x86_64)
    endif()
    add_test(NAME port_${relay_test} COMMAND port_${relay_test}_test)
  endforeach()
  add_test(NAME port_yougame_relay_gap COMMAND port_yougame_controller_relay_test gap)
  add_test(NAME port_yougame_relay_virtual COMMAND port_yougame_controller_relay_test virtual)
  add_test(NAME port_yougame_relay_ps_fixture COMMAND port_yougame_controller_relay_test ps-fixture)
endif()
