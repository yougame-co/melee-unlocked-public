# Production UI and art badge logic; only the SIS/GX boundary is captured.
set(rank_ui_stubs "${CMAKE_CURRENT_BINARY_DIR}/ranked-ui-stubs")
foreach(header mu_native.h sysdolphin/baselib/gobj.h
    sysdolphin/baselib/gobjuserdata.h sysdolphin/baselib/gobjproc.h
    sysdolphin/baselib/memory.h sysdolphin/baselib/sislib.h
    dolphin/gx.h dolphin/os.h melee/lb/lbaudio_ax.h)
  file(WRITE "${rank_ui_stubs}/${header}" "/* API facade: ranked_ui_renderer.h */\n")
endforeach()

add_executable(port_ranked_setup_motion_test port/tests/ranked_setup_motion_test.cpp)
target_include_directories(port_ranked_setup_motion_test PRIVATE "${rank_ui_stubs}"
  sourceport/game/include sourceport/game/shim port/runtime/abi "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(port_ranked_setup_motion_test PRIVATE cxx_std_17)
target_compile_options(port_ranked_setup_motion_test PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-narrowing>)
add_dependencies(port_ranked_setup_motion_test mu_rank_badge_assets)
add_test(NAME port_ranked_setup_motion COMMAND port_ranked_setup_motion_test)
# C permits the callback's implicit void* conversion; C++ needs a cast.
# Keep all renderer behavior intact when including it in the C++ fixture.
set(rank_badge_source "${CMAKE_CURRENT_SOURCE_DIR}/sourceport/game/shim/mu_rank_badge.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rank_badge_source}")
file(READ "${rank_badge_source}" rank_badge_cxx)
string(REPLACE "HSD_GObj* gobj = object;" "HSD_GObj* gobj = (HSD_GObj*)object;"
  rank_badge_cxx "${rank_badge_cxx}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ranked_ui_badge.inc" "${rank_badge_cxx}")
foreach(variant off branch)
  add_executable(port_yougame_ranked_ui_${variant}_test port/tests/yougame_ranked_ui_test.cpp)
  target_include_directories(port_yougame_ranked_ui_${variant}_test PRIVATE "${rank_ui_stubs}"
    sourceport/game/include sourceport/game/shim port/runtime/abi "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_features(port_yougame_ranked_ui_${variant}_test PRIVATE cxx_std_17)
  # The included production C uses C's implicit u8 conversions in GXColor.
  target_compile_options(port_yougame_ranked_ui_${variant}_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-narrowing>)
  if(variant STREQUAL "branch")
    target_compile_definitions(port_yougame_ranked_ui_${variant}_test PRIVATE YOUGAME_RANKED_BRANCH=1)
  endif()
  add_dependencies(port_yougame_ranked_ui_${variant}_test mu_rank_badge_assets)
  add_test(NAME port_yougame_ranked_ui_${variant} COMMAND port_yougame_ranked_ui_${variant}_test)
endforeach()
