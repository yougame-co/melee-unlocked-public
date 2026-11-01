// Raw Wii U adapter reports, including the status byte in each nine-byte port block.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "phob.h"
#include <algorithm>
namespace host::phob {
template<class Origin> HardwareInput adapter_input(const uint8_t* c, const Origin& o) {
  HardwareInput in;
  in.buttons = ((c[1] & 0x0f) << 8) | ((c[1] & 0xf0) >> 4) |
               ((c[2] & 1) << 12) | ((c[2] & 0x0e) << 3);
  in.ax = c[3]; in.ay = c[4]; in.cx = c[5]; in.cy = c[6];
  in.ax0 = o.sx; in.ay0 = o.sy; in.cx0 = o.cx; in.cy0 = o.cy;
  in.la = c[7] > o.tl ? c[7] - o.tl : 0;
  in.ra = c[8] > o.tr ? c[8] - o.tr : 0;
  return in;
}
template<class Origin> bool mirror_input(const uint8_t (&report)[37], int port, Origin& origin, HardwareInput& in) {
  if (port < 0 || port > 3 || report[0] != 0x21) return false;
  const uint8_t* c = report + 1 + port * 9;
  if (!(c[0] & 0x30)) { origin.set = false; return false; }
  if (!origin.set) origin = {true, c[3], c[4], c[5], c[6], c[7], c[8]};
  in = adapter_input(c, origin);
  return true;
}
// A current raw sample for the menu. Mirror input already passed through the controller firmware.
template<class Sample> Sample live_sample(const HardwareInput& in, uint32_t time_us) {
  Sample s{}; s.time_us = time_us;
  const auto axis = [](int a, int o) { return static_cast<int8_t>(std::clamp(a - o, -128, 127)); };
  s.raw_ax = s.ax = axis(in.ax, in.ax0); s.raw_ay = s.ay = axis(in.ay, in.ay0);
  s.raw_cx = s.cx = axis(in.cx, in.cx0); s.raw_cy = s.cy = axis(in.cy, in.cy0);
  s.raw_l = s.l = in.la; s.raw_r = s.r = in.ra; s.raw_buttons = s.buttons = in.buttons;
  s.hw_ax = in.ax; s.hw_ay = in.ay; s.hw_cx = in.cx; s.hw_cy = in.cy;
  return s;
}
// One menu-wide limiter, including socket changes. The first visible panel logs immediately.
struct MenuDiagnostic {
  double next = 0;
  bool due(bool enabled, double now) {
    if (!enabled || now < next) return false;
    next = now + 1.0; return true;
  }
};
} // namespace host::phob
