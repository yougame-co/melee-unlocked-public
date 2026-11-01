#include "yougame_unranked.h"
#include "native_slippi_bridge.h"
#include <cstdio>
#include <cstdlib>
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
  using namespace yougame::unranked;
  // Searching, game over, peer left and Z all keep the same destination; only the route matters.
  for (bool active : {false, true}) for (int mode : {-1, 0, 1, 2, 3, 4}) {
    const bool expected = active && mode == 1;
    CHECK(keep_css(active, mode) == expected);
    const bool training = mode != 0 && !expected;
    CHECK(allow_training_return(active, mode) == training);
    for (bool training_setting : {false, true})
      CHECK((training_setting && allow_training_return(active, mode)) == (training_setting && training));
  }
  using namespace slippi::online;
  uint8_t reason = 1, invalid = 2;
  CHECK(command_payload_valid(CMD_CLEANUP_CONNECTION, nullptr, 0));
  CHECK(command_payload_valid(CMD_CLEANUP_CONNECTION, &reason, 1));
  CHECK(!command_payload_valid(CMD_CLEANUP_CONNECTION, nullptr, 1));
  CHECK(!command_payload_valid(CMD_CLEANUP_CONNECTION, &invalid, 1));
  CHECK(!command_payload_valid(CMD_CLEANUP_CONNECTION, &reason, 2));
  std::printf("yougame_unranked: %d checks passed\n", checks);
}
