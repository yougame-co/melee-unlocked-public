// Slippi input suggestions: the rules shared with Slippi Dolphin, case for case with its own tests.
// Which changes are worth suggesting; how far a guess is from the real input; which suggestion fills
// each predicted frame; late and stale suggestions; a player whose suggestions keep being wrong is
// ignored, and occasional misses never add up.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_input_suggestions.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

using namespace slippi::suggestions;

struct P { uint8_t b[kPadBytes] = {}; };
static P pad(uint8_t first) { P p; p.b[0] = first; return p; }
static P stick(int x, int y) { P p; p.b[2] = (uint8_t)(int8_t)x; p.b[3] = (uint8_t)(int8_t)y; return p; }
static P c_stick(int x, int y) { P p; p.b[4] = (uint8_t)(int8_t)x; p.b[5] = (uint8_t)(int8_t)y; return p; }
static P trigger(int l) { P p; p.b[6] = (uint8_t)l; return p; }
static bool sent(const P& prev, const P& next) { return is_meaningful_change(prev.b, next.b); }
static bool suggest_and_receive(Remote& r, int32_t frame, const P& suggested, const P& real) {
  r.on_suggestion(frame, (uint32_t)frame, suggested.b);
  return r.on_real_input(frame, real.b);
}

int main() {
  // only_action_triggers_are_worth_suggesting
  CHECK(!sent(stick(0, 0), stick(22, 0)));
  CHECK(sent(stick(0, 0), stick(0, -23)));
  CHECK(!sent(stick(23, 0), stick(0, 0)));
  CHECK(!sent(stick(30, 0), stick(63, 0)));
  CHECK(sent(stick(30, 0), stick(66, 0)));
  CHECK(sent(stick(-30, 0), stick(-64, 0)));
  CHECK(!sent(stick(70, 0), stick(40, 0)));
  CHECK(!sent(stick(30, 50), stick(66, 50)));   // the unit circle pulls x back, short of a dash
  CHECK(sent(stick(0, 30), stick(0, 53)));
  CHECK(!sent(stick(0, -30), stick(0, -55)));
  CHECK(sent(stick(0, -30), stick(0, -56)));
  CHECK(sent(c_stick(30, 0), c_stick(-64, 0)));
  CHECK(sent(c_stick(0, -30), c_stick(0, -53)));
  CHECK(!sent(c_stick(0, 30), c_stick(0, 52)));
  CHECK(sent(trigger(42), trigger(43)));
  CHECK(sent(trigger(43), trigger(42)));
  CHECK(!sent(trigger(50), trigger(120)));
  CHECK(sent(pad(0), pad(0x01)));
  CHECK(sent(pad(0x10), pad(0)));
  { P up; up.b[1] = 0x08; CHECK(!sent(pad(0), up)); }      // the D-pad never
  { P z; z.b[1] = 0x10; CHECK(sent(pad(0), z)); CHECK(sent(z, pad(0))); }

  // analog_error_adds_the_worst_axis_of_each_input, wrong_buttons_ignores_bits_the_asm_does_not_compare
  {
    const uint8_t real[kPadBytes] = {0x01, 0x00, 80, 0xF0, 0, 0, 100, 10};
    const uint8_t predicted[kPadBytes] = {0x00, 0x00, 70, 0x10, 3, 0, 90, 30};
    CHECK(analog_error(predicted, real) == 45);
    CHECK(analog_error(real, real) == 0);
    const uint8_t junk[kPadBytes] = {0xE0, 0x80, 0, 0, 0, 0, 0, 0}, zero[kPadBytes] = {};
    CHECK(wrong_buttons(junk, zero) == 0);
    CHECK(wrong_buttons(pad(0x01).b, pad(0x02).b) == 0x0300);
  }

  // newest_applicable_suggestion_fills_each_slot
  {
    Remote r;
    r.on_real_input(100, pad(0).b);
    r.on_suggestion(102, 1, pad(1).b);
    r.on_suggestion(104, 2, pad(2).b);
    r.on_suggestion(102, 0, pad(9).b);   // arrives out of order but is older
    uint8_t latest[kReportBytes] = {}, out[kPredictedFrames][kReportBytes];
    latest[8] = 0x5A;   // the report's other bytes travel with the latest input, not with a suggestion
    r.fill_predictions(100, latest, out);
    const uint8_t want[kPredictedFrames] = {0, 1, 1, 2, 2, 2, 2};   // frames 101 to 107
    for (int k = 0; k < kPredictedFrames; ++k) CHECK(out[k][0] == want[k]);
    CHECK(out[0][8] == 0x5A && out[1][8] == 0);
  }
  // late_and_stale_suggestions_are_rejected
  {
    Remote r;
    r.on_real_input(100, pad(0).b);
    r.on_suggestion(100, 1, pad(1).b);
    r.on_suggestion(5000, 2, pad(1).b);
    CHECK(r.stored() == 0);
  }
  // suggestions_are_judged_only_at_their_target_frame
  {
    Remote r;
    r.on_real_input(100, pad(0).b);
    r.on_suggestion(101, 1, pad(1).b);
    uint8_t latest[kReportBytes] = {}, out[kPredictedFrames][kReportBytes];
    r.fill_predictions(100, latest, out);
    for (int k = 0; k < kPredictedFrames; ++k) CHECK(out[k][0] == 1);
    const uint8_t reals[] = {1, 1, 0, 0, 0};
    for (int i = 0; i < 5; ++i) CHECK(!r.on_real_input(101 + i, pad(reals[i]).b));
    r.on_suggestion(106, 2, pad(1).b);
    r.on_suggestion(106, 3, pad(0).b);   // pressed and released again before the poll: only the newest counts
    CHECK(!r.on_real_input(106, pad(0).b));
    // an honest player is never ignored: sixteen more wrong presses would be needed from a clean bucket
    for (int f = 107; f <= 111; ++f) CHECK(!suggest_and_receive(r, f, pad(1), pad(0)));
  }
  // wrong_suggestions_are_ignored_past_the_limit: the sixth wrong press passes 15 points
  {
    Remote r;
    r.on_real_input(100, pad(0).b);
    int started = 0, count = 0;
    for (int f = 101; f <= 110; ++f)
      if (suggest_and_receive(r, f, pad(1), pad(0))) { started = f; ++count; }
    CHECK(started == 106 && count == 1 && r.ignored());
    r.on_suggestion(112, 1000, pad(1).b);
    CHECK(r.stored() == 0);
    uint8_t latest[kReportBytes] = {}, out[kPredictedFrames][kReportBytes];
    r.fill_predictions(110, latest, out);
    for (int k = 0; k < kPredictedFrames; ++k) CHECK(out[k][0] == 0);
  }
  // occasional_misses_never_add_up_in_a_long_game
  {
    Remote r;
    r.on_real_input(100, pad(0).b);
    for (int f = 101; f <= 100100; ++f) {
      P s = pad(0);
      if (f % 400 == 0) s.b[2] = 5;
      if (f % 2000 == 1000) s.b[0] = 1;
      CHECK(!suggest_and_receive(r, f, s, pad(0)));
    }
    CHECK(!r.ignored());
  }
  // The game in progress: nothing is used when off or for a dropped player; a reset forgets a player.
  {
    uint8_t latest[kReportBytes] = {}, out[kPredictedFrames][kReportBytes];
    start_game(true, 1);
    on_remote_input(0, 100, pad(0).b);
    on_remote_suggestion(0, 101, 1, pad(1).b);
    fill_predictions(0, 100, false, latest, out);
    CHECK(out[0][0] == 1);
    fill_predictions(0, 100, true, latest, out);
    CHECK(out[0][0] == 0);
    reset_remote(0);
    fill_predictions(0, 100, false, latest, out);
    CHECK(out[0][0] == 0);
    start_game(false, 1);
    on_remote_input(0, 100, pad(0).b);
    on_remote_suggestion(0, 101, 1, pad(1).b);
    fill_predictions(0, 100, false, latest, out);
    CHECK(out[0][0] == 0);
    end_game();
    on_remote_suggestion(0, 101, 1, pad(1).b);   // no game: ignored, no crash
  }
  std::puts("slippi_input_suggestions: ok");
  return 0;
}
