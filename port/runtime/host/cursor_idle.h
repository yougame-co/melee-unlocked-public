// Cursor policy shared by the Windows and SDL game windows.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace host::cursor_idle {
constexpr uint64_t delay_ms = 2000;
inline bool visible(uint64_t now, uint64_t moved, bool focused, bool menu) {
  return !focused || menu || now - moved < delay_ms;
}
}  // namespace host::cursor_idle
