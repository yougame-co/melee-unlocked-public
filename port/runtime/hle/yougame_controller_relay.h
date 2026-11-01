// The app's relayed pads in the game's own controller families (host/controller_relay.h): a
// PlayStation or Switch pad gets its family's bindings, options, capture and port choices instead
// of the Xbox ones. Xbox and other pads stay the Xbox family; "hid" keeps its own 32-bit mask.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_relay_pads.h"
#include "controller_relay.h"

namespace yougame::relay_pads {
inline host::CaptureDevice capture_family(Family family) {
  switch (family) {
    case Family::PlayStation: return host::CaptureDevice::DS4Pad;
    case Family::Switch: return host::CaptureDevice::SwitchPro;
    case Family::Hid: return host::CaptureDevice::HidPad;
    default: return host::CaptureDevice::XInputPad;
  }
}

// Resolve the actual routed family/index back to the app's seat, including sparse and mixed lists.
inline int seat_for(const Frame& frame, host::CaptureDevice family, int index) {
  if (family == host::CaptureDevice::None || index < 0 || index >= 4) return -1;
  for (unsigned seat = 0; seat < frame.pads.size(); ++seat) {
    const auto& p = frame.pads[seat];
    if (p.present && p.family_index == unsigned(index) && capture_family(family_of(p.kind)) == family)
      return int(seat);
  }
  return -1;
}
inline std::string family_line(unsigned seat, const Pad& pad, int port) {
  if (pad.kind != "ps" && pad.kind != "switch" && pad.kind != "xbox") return {};
  const auto family = family_of(pad.kind);
  const char* bindings = family == Family::PlayStation ? "DS4" : family == Family::Switch ? "SWPRO" : "XInput";
  return "relay pads: slot " + std::to_string(seat) + " kind " + pad.kind + " family " +
      std::to_string(pad.family_index) + " -> " + bindings + " bindings, port " + std::to_string(port + 1);
}
// Keep the first (lowest) actual port for each seat. Duplicate explicit routes must not
// alternate the logged port every poll. A gap or a reassignment starts a new announcement.
class RouteLog {
 public:
  void begin(const Frame& frame) {
    for (unsigned seat = 0; seat < last_.size(); ++seat) {
      auto& last = last_[seat];
      if (!last.seen || !frame.pads[seat].present) last = {};
      last.seen = false;
    }
  }
  std::string changed(const Frame& frame, host::CaptureDevice family, int index, int port) {
    if (port < 0 || port >= 4) return {};
    const int seat = seat_for(frame, family, index);
    if (seat < 0) return {};
    auto& last = last_[seat];
    if (last.seen) return {};
    last.seen = true;
    const auto& pad = frame.pads[seat];
    if (last.id == pad.id && last.kind == pad.kind && last.index == pad.family_index && last.port == port)
      return {};
    last.id = pad.id; last.kind = pad.kind; last.index = pad.family_index; last.port = port;
    return family_line(unsigned(seat), pad, port);
  }
 private:
  struct Last {
    std::string id, kind;
    unsigned index = kNoFamilyIndex;
    int port = -1;
    bool seen = false;
  };
  std::array<Last, 4> last_{};
};
// Game poll owns the cache: a single stdin frame serves all family reads and its routing log.
inline thread_local Frame game_frame;

// One sample per present pad, at its seat, numbered by its familyIndex (a gap stays a gap).
inline host::controller_relay::Snapshot controller_snapshot(const Frame& frame) {
  host::controller_relay::Snapshot result;
  const bool has_real_pad = std::any_of(frame.pads.begin(), frame.pads.end(), [](const Pad& p) {
    return p.present && !(p.kind == "other" && p.id == "virtual");
  });
  for (size_t seat = 0; seat < frame.pads.size(); ++seat) {
    const Pad& p = frame.pads[seat];
    if (!p.present || p.family_index >= 4) continue;   // a skipped duplicate stays out of the game
    const Family family = family_of(p.kind);
    host::controller_relay::Sample& s = result.devices[seat];
    s.family = capture_family(family);
    s.index = static_cast<int>(p.family_index);
    s.name = p.name;
    s.keyboard_standin = p.kind == "other" && p.id == "virtual";
    // The page keyboard may play alone, but must not take automatic P1 from a connected pad.
    s.allow_fallback = !(has_real_pad && p.kind == "other" && p.id == "virtual");
    s.axes[0] = p.lx; s.axes[1] = p.ly; s.axes[2] = p.rx; s.axes[3] = p.ry;
    s.left = p.lt; s.right = p.rt;
    if (family == Family::Hid) { s.buttons = p.buttons; continue; }
    // The app's buttons are XInput positions (south = 0x1000): the family's own mask, L2/R2 or ZL/ZR
    // from the trigger values. Xbox is unchanged.
    s.buttons = host::controller_relay::native_buttons(static_cast<uint16_t>(p.buttons), s.family, p.lt, p.rt);
    if (family == Family::PlayStation) {
      // As playstation_pad.cpp reads a pad: no trigger travel below the L2/R2 press point.
      if (s.left <= host::kPlayStationTriggerPress) s.left = 0;
      if (s.right <= host::kPlayStationTriggerPress) s.right = 0;
    } else if (family == Family::Switch) {
      // ZL and ZR are buttons, as switch_pro.cpp reads a Pro Controller: no travel. Any travel here
      // reaches the game through swpro_apply_bindings: ZR (Z by default) would also press R all the
      // way in, and the unbound ZL would shield.
      s.left = s.right = 0;
    }
  }
  return result;
}
}  // namespace yougame::relay_pads
