// App answers never request a window change; only paced player choices do.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_embed_fullscreen.h"
#include "yougame_parity.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using yougame::embed_fullscreen::Line;
using yougame::embed_fullscreen::State;
static std::vector<std::string> lines;
static constexpr uint64_t G = State::kGapMs;

int main() {
  // Caps is always first, includes fullscreen, and starts once on either route.
  for (bool toplevel : {false, true}) {
    yougame::parity::Signals signals;
    lines.clear();
    signals.start(false, [](const char* line) { lines.emplace_back(line); }, toplevel);
    signals.emit("yg {\"fullscreen\":1}");
    CHECK(lines.empty());
    signals.start(true, [](const char* line) { lines.emplace_back(line); }, toplevel);
    signals.start(true, [](const char* line) { lines.emplace_back(line); }, toplevel);
    signals.emit("yg {\"fullscreen\":1}");
    CHECK(lines.size() == 2);
    CHECK(lines.front() == yougame::parity::caps(toplevel));
    CHECK(lines.front().find("\"fullscreen\"") != std::string::npos);
  }
  State gate;
  CHECK(gate.display(true) == -1);
  CHECK(gate.request(true, true, 0) == -1);
  CHECK(gate.flush(true, 1000) == -1);
  for (const char* bad : {"fullscreen", "fullscreen ", "fullscreen 2", "fullscreen -1",
                         "fullscreen 01", "fullscreen 1 ", "fullscreen 0 junk", "fullscreen\t1", "fullscreenx 1"}) {
    CHECK(gate.receive(bad) == Line::Invalid);
    CHECK(gate.display(true) == -1);
  }
  CHECK(gate.receive(std::string("fullscreen 1\0", 13)) == Line::Invalid);
  CHECK(gate.receive("view 1 2 3 4") == Line::Other);
  CHECK(gate.receive("fullscreen 1") == Line::Accepted);
  CHECK(gate.display(false) == -1);
  CHECK(gate.request(false, false, 0) == -1);
  CHECK(gate.display(true) == 1);
  CHECK(gate.flush(true, 0) == -1);
  CHECK(gate.receive("fullscreen 1") == Line::Accepted);
  CHECK(gate.flush(true, 1000) == -1);
  CHECK(gate.request(true, false, 1000) == 0);
  CHECK(gate.display(true) == 1); // Requests are not optimistic app answers.
  CHECK(gate.request(true, true, 1001) == -1);
  CHECK(gate.receive("fullscreen 0") == Line::Accepted); // The answer to the 0 request.
  CHECK(gate.display(true) == 0);
  CHECK(gate.flush(true, 1000 + G - 1) == -1);
  CHECK(gate.flush(true, 1000 + G) == 1); // The player's last choice still goes out.
  CHECK(gate.receive("fullscreen 0") == Line::Accepted); // The app refuses it: follow the 0.
  CHECK(gate.display(true) == 0);
  CHECK(gate.flush(true, 4250) == -1); // No retry after the app's refusal.
  CHECK(gate.request(true, true, 4250) == 1); // A fresh player choice is allowed.
  CHECK(gate.request(true, false, 4251) == -1);
  CHECK(gate.receive("fullscreen 0") == Line::Accepted); // Repeated answers cancel too.
  CHECK(gate.flush(true, 4500) == -1);

  State quick; // On, then Window within the gap: Window wins even after the app enters full screen.
  CHECK(quick.receive("fullscreen 0") == Line::Accepted);
  CHECK(quick.request(true, true, 0) == 1);
  CHECK(quick.request(true, false, 100) == -1);
  CHECK(quick.receive("fullscreen 1") == Line::Accepted);
  CHECK(quick.flush(true, G - 1) == -1);
  CHECK(quick.flush(true, G) == 0);
  CHECK(quick.receive("fullscreen 0") == Line::Accepted);
  CHECK(quick.flush(true, 600) == -1); // Nothing in reaction to the answer.

  State paced;
  CHECK(paced.receive("fullscreen 0") == Line::Accepted);
  CHECK(paced.request(true, true, 0) == 1);
  CHECK(paced.request(true, false, 1) == -1);
  CHECK(paced.request(true, true, 2) == -1);
  CHECK(paced.request(true, false, G - 1) == -1);
  CHECK(paced.flush(true, G - 1) == -1);
  CHECK(paced.flush(true, G) == 0); // Last wins even with the overlay closed.
  CHECK(paced.flush(true, 2 * G) == -1);
  std::vector<uint64_t> sent{0, G};
  for (uint64_t t = 2 * G + 1; t < 2501; ++t) {
    const int result = paced.request(true, t % 2, t);
    if (result >= 0) { CHECK(result == int(t % 2)); sent.push_back(t); }
  }
  for (size_t i = 1; i < sent.size(); ++i) CHECK(sent[i] - sent[i - 1] >= G);
  for (size_t i = 4; i < sent.size(); ++i) CHECK(sent[i] - sent[i - 4] >= 1000);
  const uint64_t last = sent.back();
  CHECK(last != 2500); // The loop ends on an unsent choice (0).
  CHECK(paced.flush(true, last + G) == 0); // Last unsent choice survives to a later frame.
  CHECK(paced.flush(true, last + 2 * G) == -1);
  CHECK(paced.request(true, true, last + 2 * G) == 1);
  CHECK(paced.request(true, false, last + 2 * G + 1) == -1);
  CHECK(paced.flush(false, last + 3 * G) == -1);
  CHECK(paced.flush(true, last + 4 * G) == -1);

  State shown; // The Display option shows the player's choice until the app answers it.
  CHECK(shown.shown(true, 0) == -1);
  CHECK(shown.receive("fullscreen 0") == Line::Accepted);
  CHECK(shown.shown(false, 0) == -1);
  CHECK(shown.shown(true, 0) == 0);
  CHECK(shown.request(true, true, 1000) == 1);
  CHECK(shown.shown(true, 1700) == 1); // Mac's full-screen animation: no answer yet.
  CHECK(shown.shown(true, 1000 + State::kShowMs) == 0); // Unanswered too long: the app's state.
  CHECK(shown.receive("fullscreen 1") == Line::Accepted);
  CHECK(shown.shown(true, 3500) == 1);
  CHECK(shown.request(true, false, 4000) == 0);
  CHECK(shown.shown(true, 4001) == 0);
  CHECK(shown.receive("fullscreen 1") == Line::Accepted); // Any answer ends the wait.
  CHECK(shown.shown(true, 4002) == 1);
  CHECK(shown.request(true, false, 4000 + G - 1) == -1);
  CHECK(shown.shown(true, 4000 + G - 1) == 0); // A queued choice shows at once.
  State refused;
  CHECK(refused.receive("fullscreen 0") == Line::Accepted);
  CHECK(refused.request(true, true, 0) == 1);
  CHECK(refused.shown(true, 10) == 1);
  CHECK(refused.receive("fullscreen 0") == Line::Accepted); // Refused: Window at once.
  CHECK(refused.shown(true, 20) == 0);
  CHECK(refused.flush(true, 5000) == -1);
  std::printf("yougame_embed_fullscreen: %d checks passed\n", checks);
}
