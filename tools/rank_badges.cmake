# Included by both the host tests and the separately configured native game.
# The checked-in PNGs are the image source; no YouGame checkout is needed.
get_filename_component(MU_RANK_ART_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(MU_RANK_ASSETS "${MU_RANK_ART_ROOT}/sourceport/game/shim/assets/mu_rank_badges.h")
file(GLOB MU_RANK_PNGS CONFIGURE_DEPENDS "${MU_RANK_ART_ROOT}/sourceport/game/shim/assets/rank_badges/*.png")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mu-rank-badges.stamp"
  COMMAND "${Python3_EXECUTABLE}" -B "${MU_RANK_ART_ROOT}/tools/rasterize_rank_badges.py"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/mu-rank-badges.stamp"
  DEPENDS "${MU_RANK_ART_ROOT}/tools/rasterize_rank_badges.py"
    ${MU_RANK_PNGS}
    "${MU_RANK_ASSETS}"
  VERBATIM)
add_custom_target(mu_rank_badge_assets DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mu-rank-badges.stamp")
