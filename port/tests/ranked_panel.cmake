# Execute the production panel with fake SIS/badge allocation and cached commands.
# MinGW separately checks these functions against the real game headers/ABI.
set(rank_panel_stubs "${CMAKE_CURRENT_BINARY_DIR}/ranked-panel-stubs")
foreach(header mu_native.h sysdolphin/baselib/gobj.h
    sysdolphin/baselib/gobjuserdata.h sysdolphin/baselib/sislib.h)
  file(WRITE "${rank_panel_stubs}/${header}" "/* API facade declared by ranked_panel_test.cpp */\n")
endforeach()
set(rank_splash_source "${CMAKE_CURRENT_SOURCE_DIR}/sourceport/game/shim/mu_slippi_splash.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rank_splash_source}")
file(READ "${rank_splash_source}" rank_splash_text)
string(FIND "${rank_splash_text}" "    if (mu_slippi_state()->mode == MU_SLP_MODE_RANKED) {" rank_splash_start)
string(FIND "${rank_splash_text}" "    /* Stage name */" rank_splash_end)
if(rank_splash_start LESS 0 OR rank_splash_end LESS rank_splash_start)
  message(FATAL_ERROR "Ranked splash fixture boundaries changed")
endif()
math(EXPR rank_splash_length "${rank_splash_end} - ${rank_splash_start}")
string(SUBSTRING "${rank_splash_text}" ${rank_splash_start} ${rank_splash_length} rank_splash)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ranked_splash_score.inc" "${rank_splash}\n")
add_executable(port_yougame_ranked_panel_test port/tests/yougame_ranked_panel_test.cpp)
target_include_directories(port_yougame_ranked_panel_test PRIVATE "${rank_panel_stubs}"
  sourceport/game/include port/runtime/abi "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(port_yougame_ranked_panel_test PRIVATE cxx_std_17)
add_test(NAME port_yougame_ranked_panel COMMAND port_yougame_ranked_panel_test)
add_executable(port_yougame_ranked_panel_branch_test port/tests/yougame_ranked_panel_test.cpp)
target_include_directories(port_yougame_ranked_panel_branch_test PRIVATE "${rank_panel_stubs}"
  sourceport/game/include port/runtime/abi "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(port_yougame_ranked_panel_branch_test PRIVATE cxx_std_17)
target_compile_definitions(port_yougame_ranked_panel_branch_test PRIVATE YOUGAME_RANKED_BRANCH=1)
add_test(NAME port_yougame_ranked_panel_branch COMMAND port_yougame_ranked_panel_branch_test)
