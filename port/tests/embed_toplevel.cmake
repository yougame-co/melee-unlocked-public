# Synthetic Windows policy and wiring coverage, before the Linux return().
add_executable(port_yougame_embed_toplevel_test "${PROJECT_SOURCE_DIR}/port/tests/yougame_embed_toplevel_test.cpp")
target_include_directories(port_yougame_embed_toplevel_test PRIVATE "${PROJECT_SOURCE_DIR}/port/runtime/hle")
target_compile_features(port_yougame_embed_toplevel_test PRIVATE cxx_std_17)
add_test(NAME port_yougame_embed_toplevel COMMAND port_yougame_embed_toplevel_test)
add_test(NAME port_yougame_embed_toplevel_wiring COMMAND "${Python3_EXECUTABLE}" -B
  "${PROJECT_SOURCE_DIR}/port/tests/yougame_embed_toplevel_wiring_test.py")
