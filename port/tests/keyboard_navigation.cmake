# Real binding application and ImGui feed, registered before the native Linux return too.
add_executable(port_keyboard_navigation_test
  "${PROJECT_SOURCE_DIR}/port/tests/keyboard_navigation_test.cpp"
  "${PROJECT_SOURCE_DIR}/port/third_party/imgui/imgui.cpp"
  "${PROJECT_SOURCE_DIR}/port/third_party/imgui/imgui_draw.cpp"
  "${PROJECT_SOURCE_DIR}/port/third_party/imgui/imgui_tables.cpp"
  "${PROJECT_SOURCE_DIR}/port/third_party/imgui/imgui_widgets.cpp")
target_include_directories(port_keyboard_navigation_test PRIVATE
  "${PROJECT_SOURCE_DIR}/port/runtime/host" "${PROJECT_SOURCE_DIR}/port/runtime/ppc"
  "${PROJECT_SOURCE_DIR}/port/runtime/gx" "${PROJECT_SOURCE_DIR}/port/third_party/imgui")
if(NOT WIN32)
  target_include_directories(port_keyboard_navigation_test PRIVATE "${PROJECT_SOURCE_DIR}/port/linux/compat")
endif()
if(APPLE)
  set_target_properties(port_keyboard_navigation_test PROPERTIES OSX_ARCHITECTURES x86_64)
endif()
target_compile_definitions(port_keyboard_navigation_test PRIVATE NOMINMAX)
target_compile_features(port_keyboard_navigation_test PRIVATE cxx_std_17)
add_test(NAME port_keyboard_navigation COMMAND port_keyboard_navigation_test)
add_test(NAME port_keyboard_navigation_wiring COMMAND "${Python3_EXECUTABLE}" -B
  "${PROJECT_SOURCE_DIR}/port/tests/keyboard_navigation_wiring_test.py")
