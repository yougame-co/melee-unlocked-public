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
    {Provider::YouGame, "Eve", 15, "Eve"},
    {Provider::Slippi, "Eve", 15, "Eve - Slippi"},
    {Provider::Unknown, "Eve", 15, "Eve"},
    {Provider::Slippi, "TAG#123", 15, "TAG#12 - Slippi"},
    {Provider::YouGame, "TAG#123", 15, "TAG#123"},
    {Provider::Unknown, "TAG#123", 15, "TAG#123"},
    {Provider::Slippi, "LongOpponentName", 15, "LongOp - Slippi"},
    {Provider::YouGame, "LongOpponentName", 15, "LongOpponentNam"},
    {Provider::Unknown, "LongOpponentName", 4, "Long"},
    {Provider::Slippi, "TAG#123", 0, ""},
    {Provider::Slippi, "TAG#123", 8, ""},
    {Provider::Slippi, "TAG#123", 9, " - Slippi"},
    {Provider::Slippi, "TAG#123", 10, "T - Slippi"},
    {Provider::YouGame, "Eve", 0, ""},
    {Provider::Unknown, "Eve", 0, ""},
    {Provider::Slippi, "", 15, ""},
    {Provider::YouGame, "", 15, ""},
    {Provider::Unknown, "", 15, ""},
    {Provider::Slippi, u8"あいうえお", 11, u8"あい - Slippi"},
    {Provider::YouGame, u8"あいうえお", 3, u8"あいう"},
    {Provider::Unknown, u8"あいうえお", 3, u8"あいう"},
    {Provider::Slippi, u8"A😀B", 11, u8"A😀 - Slippi"},
    {Provider::Unknown, u8"A😀B", 2, u8"A😀"},
    {Provider::Slippi, "Opponent", std::string::npos, "Opponent - Slippi"},
    {Provider::YouGame, "Opponent (TAG#123)", std::string::npos, "Opponent (TAG#123)"},
  };
  for (const auto& test : cases)
    CHECK(opponent_label(test.provider, test.name, test.width) == test.expected);
  // Labeling a copy must not modify the raw identity used for tickets and replay metadata.
  const std::string raw = "Opponent";
  CHECK(opponent_label(Provider::Slippi, raw) == "Opponent - Slippi");
  CHECK(raw == "Opponent");
  std::printf("opponent_display: %zu label cases + 2 identity checks passed\n", sizeof(cases) / sizeof(cases[0]));
}
