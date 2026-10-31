# Window-free boot diagnostics. All image bytes are synthetic fixtures made by the test.
add_executable(port_disc_image_reader_test "${PROJECT_SOURCE_DIR}/port/tests/disc_image_reader_test.cpp")
target_include_directories(port_disc_image_reader_test PRIVATE "${PROJECT_SOURCE_DIR}/port/runtime/host")
target_compile_features(port_disc_image_reader_test PRIVATE cxx_std_17)
if(WIN32)
  target_compile_definitions(port_disc_image_reader_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME port_disc_image_reader COMMAND port_disc_image_reader_test)

add_executable(port_settings_log_test "${PROJECT_SOURCE_DIR}/port/tests/settings_log_test.cpp")
target_include_directories(port_settings_log_test PRIVATE "${PROJECT_SOURCE_DIR}/port/app")
target_compile_features(port_settings_log_test PRIVATE cxx_std_17)
add_test(NAME port_settings_log COMMAND port_settings_log_test)
