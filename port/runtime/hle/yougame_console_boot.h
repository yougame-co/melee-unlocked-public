// Boot-only handoff; waits never enter simulation/retrace.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console.h"
#include "yougame_console_notice.h"
#include <future>
#include <chrono>
namespace yougame::console {
template<class Work, class Pump> auto responsive_wait(Work work, Pump pump) {
  auto result = std::async(std::launch::async, std::move(work));
  while (result.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) pump();
  pump();
  return result.get();
}
// False means ordinary local boot, including a rejected/failed console request.
template<class Start, class Begin, class Ready, class Pump, class Notice, class Log>
bool boot(const std::optional<nlohmann::json>& request, Start start, Begin begin,
          Ready ready, Pump pump, Notice notice, Log log) {
  if (!request) return false;
  ready(); // caps/ready must precede the potentially 50-second room/link wait
  SessionConfig config; std::string error;
  auto link = responsive_wait([&] { return start(*request, config, error); }, pump);
  if (link && begin(std::move(link), config)) return true;
  if (error.empty()) error = "couldn't prepare the console session";
  log("console: local play after start failure: " + error);
  if (auto message = session_notice(error)) notice(message);
  else notice(("Couldn't start playing together: " + error).c_str());
  return false;
}
}
