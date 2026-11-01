// Agent-only console room request, alongside Friends' staging startup defaults.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_console_request.h"
namespace yougame::agent_test {
inline bool console_key(const nlohmann::json& value, nlohmann::json& out) {
  if (!value.is_object() || value.size() != 2 || !value.count("room") || !value.count("players")) return false;
  return console::parse_request(value,out);
}
}
#endif
