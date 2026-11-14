// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#pragma once
#if MU_SQUAD
#include "../abi/mu_squad_wire.h"
namespace slippi::squad_test {
// Decimal only, no signs, whitespace, empty fields or trailing separators.
inline bool parse(const char* text, MuSquadSide* out) {
  if (!text || !out) return false;
  MuSquadSide value{};
  const char* p = text;
  auto number = [&p](unsigned& n) {
    n = 0;
    if (*p < '0' || *p > '9') return false;
    do { n = n * 10 + unsigned(*p++ - '0'); if (n > 255) return false; }
    while (*p >= '0' && *p <= '9');
    return true;
  };
  for (;;) {
    unsigned kind, color;
    if (value.count == MU_SQUAD_LINEUP || !number(kind) || *p++ != '/' || !number(color) ||
        kind > 25 || color > 5) return false;
    value.members[value.count++] = {(unsigned char)kind, (unsigned char)color};
    if (!*p) break;
    if (*p++ != ',') return false;
  }
  // Apply the same per-fighter costume range as the production lineup command.
  if (!mu_squad_side_valid(&value)) return false;
  *out = value;
  return true;
}
} // namespace slippi::squad_test
#endif
