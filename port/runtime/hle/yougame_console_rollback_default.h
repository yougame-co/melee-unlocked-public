// Single test-build default. Explicit environment values 0 and 1 override either build.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>

namespace yougame::console {
constexpr bool kConsoleRollbackDefault = true;
inline bool rollback_setting(const char* value) {
  if (value && std::strcmp(value, "1") == 0) return true;
  if (value && std::strcmp(value, "0") == 0) return false;
  return kConsoleRollbackDefault;
}
} // namespace yougame::console
