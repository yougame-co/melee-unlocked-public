// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#pragma once
#if MU_SQUAD
#include "../abi/mu_host.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace host::squad_digest {
// Body numbers address input history. match_frame addresses the existing digest;
// they need not share an origin (the VS counter can stand still during startup).
struct Row { int32_t body; uint32_t retrace; MuStatePod state; };
// The digest fields write() emits (rng .. match_frame); later MuStatePod fields are not compared.
inline bool same_digest(const MuStatePod& a, const MuStatePod& b) {
  return a.rng == b.rng && a.scene == b.scene && a.scene_major == b.scene_major &&
         a.match_frame == b.match_frame && !std::memcmp(a.player, b.player, sizeof a.player);
}
// Lateness is a body question: a body at or below the confirmed frontier is final. A later
// sample for it ("final when written" broken) is compared with that body's confirmed state,
// never looked up by match_frame, which can stand still (startup, GO, GAME!, pause).
enum class Late { none, same, different, unknown };
struct LateRow { int32_t body; uint32_t match_frame, confirmed_match_frame; int32_t frontier_at_confirm; };
class Pending {
  static constexpr size_t kKeepBodies = 128;
  struct Final { MuStatePod state; int32_t frontier; };
  std::map<uint32_t, Row> rows;           // by match_frame: what the confirmed CSV writes
  std::map<int32_t, MuStatePod> bodies;   // last simulation of every not-yet-final body
  std::map<int32_t, Final> recent;        // the last kKeepBodies final bodies
  uint32_t written = 0;
  int32_t final_through = -1, last_body = -1;
  uint32_t last_match_frame = 0, held_bodies = 0;
public:
  Late sample(int32_t body, uint32_t retrace, const MuStatePod& state, LateRow* late = nullptr) {
    if (body < 0) return Late::none;
    if (body <= final_through) {
      const auto found = recent.find(body);
      if (found == recent.end()) return Late::unknown; // older than the kept window
      if (same_digest(found->second.state, state)) return Late::same;
      if (late) *late = {body, state.match_frame, found->second.state.match_frame, found->second.frontier};
      return Late::different;
    }
    if (body > last_body) {
      if (last_body >= 0 && state.match_frame == last_match_frame) ++held_bodies;
      last_body = body;
      last_match_frame = state.match_frame;
    }
    bodies[body] = state;
    if (state.match_frame > written) // a held counter never re-emits a written frame
      rows[state.match_frame] = {body, retrace, state}; // LAST simulation, including resim
    return Late::none;
  }
  template<class Emit> void confirm(int32_t frontier, Emit emit) {
    if (frontier > final_through) final_through = frontier;
    while (!bodies.empty() && bodies.begin()->first <= frontier) {
      recent[bodies.begin()->first] = {bodies.begin()->second, frontier};
      bodies.erase(bodies.begin());
    }
    while (recent.size() > kKeepBodies) recent.erase(recent.begin());
    while (!rows.empty() && rows.begin()->second.body <= frontier) {
      const auto row = rows.begin()->second;
      written = row.state.match_frame;
      rows.erase(rows.begin());
      emit(row);
    }
  }
  void discard_tail() { rows.clear(); bodies.clear(); }
  uint32_t held() const { return held_bodies; } // new bodies on an unchanged match_frame
};
inline void header(FILE* file) {
  std::fprintf(file, "frame,rng,scene");
  for (unsigned slot = 0; slot < 6; ++slot)
    for (const char* field : {"present", "stocks", "action", "anim_frame", "pos_x", "pos_y", "pos_z",
                              "vel_x", "vel_y", "vel_z", "percent", "facing"})
      std::fprintf(file, ",p%u_%s", slot, field);
  std::fprintf(file, ",scene_major,match_frame\n");
}
inline void late_header(FILE* file) {
  std::fprintf(file, "body,match_frame,confirmed_match_frame,frontier_at_confirm\n");
}
inline void write_late(FILE* file, const LateRow& late) {
  std::fprintf(file, "%d,%u,%u,%d\n", late.body, late.match_frame, late.confirmed_match_frame,
               late.frontier_at_confirm);
}
inline void write(FILE* file, const Row& row) {
  const auto& state = row.state;
  std::fprintf(file, "%u,%08X,%08X", row.retrace, state.rng, state.scene);
  for (const auto& f : state.player) {
    const uint32_t* words = &f.present;
    for (unsigned index = 0; index < 12; ++index) std::fprintf(file, ",%08X", words[index]);
  }
  std::fprintf(file, ",%08X,%08X\n", state.scene_major, state.match_frame);
}
} // namespace host::squad_digest
#endif
