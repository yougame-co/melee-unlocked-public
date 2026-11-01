// Diagnostic environment overrides never affect YouGame sessions. Without YouGame they read as
// upstream reads them, --vanilla-only included.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gameplay_policy.h"
#include <cstdlib>
namespace host::test_env {
// Latched from yougame::active() by start(), before diagnostic readers or workers.
inline bool yougame_session = false;
// Set by main() from a valid agent-test.json. Staging builds all carry YOUGAME_AGENT_TEST_HOOK,
// so the build flag alone must never mark a player's copy as an agent's.
inline bool agent_run = false;
inline bool agent_copy() {
  if (agent_run) return true;
  const char* label = std::getenv("YOUGAME_TEST_LABEL");
  return label && *label;
}
inline const char* getenv(const char* name) {
  return yougame_session ? nullptr : std::getenv(name);
}
} // namespace host::test_env
