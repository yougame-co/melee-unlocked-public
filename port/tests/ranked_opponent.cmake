# Compile the actual B3 rank fill with a socket-free YouGame facade. Keep this
# above the root Linux return(), so Linux exercises the same seat mapping.
set(rank_online_source "${CMAKE_CURRENT_SOURCE_DIR}/port/runtime/hle/slippi_online.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rank_online_source}")
file(READ "${rank_online_source}" rank_online_text)
string(FIND "${rank_online_text}" "  std::array<uint8_t, 2> ranks{};" rank_fill_start)
string(FIND "${rank_online_text}" "  q.push_back(ranks[1]);" rank_fill_end)
if(rank_fill_start LESS 0 OR rank_fill_end LESS rank_fill_start)
  message(FATAL_ERROR "B3 opponent-rank fixture boundaries changed")
endif()
string(LENGTH "  q.push_back(ranks[1]);" rank_fill_tail)
math(EXPR rank_fill_length "${rank_fill_end} + ${rank_fill_tail} - ${rank_fill_start}")
string(SUBSTRING "${rank_online_text}" ${rank_fill_start} ${rank_fill_length} rank_fill)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ranked_b3_fill.inc" "${rank_fill}\n")
add_executable(port_yougame_ranked_opponent_test port/tests/yougame_ranked_opponent_test.cpp)
target_include_directories(port_yougame_ranked_opponent_test PRIVATE
  port/runtime/hle port/third_party "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(port_yougame_ranked_opponent_test PRIVATE cxx_std_20)
add_test(NAME port_yougame_ranked_opponent COMMAND port_yougame_ranked_opponent_test)
