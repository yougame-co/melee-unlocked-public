#pragma once

namespace host {

// Latency trace (MELEE_TRACE_LATENCY=1): when a simulation tick was due and when it woke, when the
// game first read its pads in it, and how old the newest adapter report was at that read (-1 when
// no adapter report was read). Seconds on the now_seconds clock, except pad_age in milliseconds.
// Written by the simulation thread and copied into the frame it produces.
struct TickTiming {
  double target = 0, wake = 0, pad = 0, pad_age = -1;
  // Latency probe (the trace with an input script): the number of the newest press the script
  // injected (0 none yet), when it counts as injected (the scheduled start of the tick that read
  // it) and when that tick read its pads. Carried from tick to tick, so the first frame presented
  // with the press in it reports it even when the press's own frame was never drawn.
  unsigned probe = 0;
  double probe_inject = 0, probe_pad = 0;
};

}  // namespace host
