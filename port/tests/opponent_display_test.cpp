// Window-free presentation checks: no account, game, replay or network access.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "opponent_display.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
  using namespace slippi::display;
  struct Case { Provider provider; const char* name; size_t width; const char* expected; };
  const Case cases[] = {
    {Provider::Slippi, "TAG#123", 15, "Slippi TAG#123"},
    {Provider::YouGame, "TAG#123", 15, "YouGame TAG#123"},
    {Provider::Unknown, "TAG#123", 15, "TAG#123"},
    {Provider::Slippi, "LongOpponentName", 15, "Slippi LongOppo"},
    {Provider::YouGame, "LongOpponentName", 15, "YouGame LongOpp"},
    {Provider::Unknown, "LongOpponentName", 4, "Long"},
    {Provider::Slippi, "TAG#123", 0, ""},
    {Provider::Slippi, "TAG#123", 6, "Slippi"},
    {Provider::Slippi, "", 15, "Slippi"},
    {Provider::YouGame, "", 15, "YouGame"},
    {Provider::Unknown, "", 15, ""},
    {Provider::Slippi, u8"あいうえお", 9, u8"Slippi あい"},
    {Provider::YouGame, u8"あいうえお", 9, u8"YouGame あ"},
    {Provider::Unknown, u8"あいうえお", 3, u8"あいう"},
    {Provider::Unknown, u8"A😀B", 2, u8"A😀"},
    {Provider::Slippi, "Opponent", std::string::npos, "Slippi Opponent"},
    {Provider::YouGame, "Opponent (TAG#123)", std::string::npos, "YouGame Opponent (TAG#123)"},
  };
  for (const auto& test : cases)
    CHECK(opponent_label(test.provider, test.name, test.width) == test.expected);
  // Labeling a copy must not modify the raw identity used for tickets and replay metadata.
  const std::string raw = "Opponent";
  CHECK(opponent_label(Provider::Slippi, raw) == "Slippi Opponent");
  CHECK(raw == "Opponent");
  std::printf("opponent_display: %zu label cases + 2 identity checks passed\n", sizeof(cases) / sizeof(cases[0]));
}
