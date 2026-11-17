// Arena room rules (see arena_rules.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_rules.h"
#include <algorithm>
#include <cctype>

namespace slippi::arena {
namespace {

constexpr uint8_t kWireVersion = 2;

std::string clean(const std::string& s, size_t max, bool upper) {
  std::string out;
  for (char c : s) {
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || u == 0x7F) continue;
    out += upper ? (char)std::toupper(u) : c;
    if (out.size() >= max) break;
  }
  return out;
}

bool is_fighting(const Room& r, uint8_t id) { return ring_slot(r, id) >= 0; }

void remove_from_line(Room& r, uint8_t id) {
  r.line.erase(std::remove(r.line.begin(), r.line.end(), id), r.line.end());
}

uint8_t pop_front(Room& r) {
  if (r.line.empty()) return kNone;
  const uint8_t id = r.line.front();
  r.line.erase(r.line.begin());
  return id;
}

void clear_choices(Room& r) {
  r.asker = kNone; r.refused = 0;
  r.leaving_slot = kNone; r.incoming = kNone; r.leaver_plays_winner = false;
}

// Keeps the ring full when it can be: an empty slot takes the front of the line. Doubles that can no
// longer fill four slots go back to singles (the first fighters in slot order stay, the others go to
// the front of the line).
void normalize(Room& r) {
  const int n = ring_size(r);
  for (int s = 0; s < n; ++s)
    if (r.ring[s] == kNone && !r.line.empty()) { r.ring[s] = pop_front(r); r.tenure[s] = 0; }
  if (r.doubles) {
    bool full = true;
    for (int s = 0; s < 4; ++s) if (r.ring[s] == kNone) full = false;
    if (!full) {
      std::vector<uint8_t> in;
      for (int s = 0; s < 4; ++s) if (r.ring[s] != kNone) in.push_back(r.ring[s]);
      r.doubles = false;
      for (int s = 0; s < 4; ++s) { r.ring[s] = kNone; r.tenure[s] = 0; }
      for (size_t i = 0; i < in.size(); ++i) {
        if (i < 2) r.ring[i] = in[i];
        else r.line.insert(r.line.begin() + (std::ptrdiff_t)(i - 2), in[i]);
      }
      r.switch_pending = false; r.switch_votes = 0;
      clear_choices(r);
      r.streak_id = kNone; r.streak = 0;
    }
  }
  // Someone who asked, or was chosen to come in, and got into the ring anyway: that is settled.
  if (r.asker != kNone && ring_slot(r, r.asker) >= 0) { r.asker = kNone; r.refused = 0; }
  if (r.incoming != kNone && ring_slot(r, r.incoming) >= 0) r.incoming = kNone;
  if (r.leaving_slot != kNone && (r.leaving_slot >= ring_size(r) || r.ring[r.leaving_slot] == kNone)) {
    r.leaving_slot = kNone; r.incoming = kNone; r.leaver_plays_winner = false;
  }
  if (r.switch_pending && switch_voters(r).empty()) { r.switch_pending = false; r.switch_votes = 0; }
}

bool switch_complete(const Room& r) {
  if (!r.switch_pending) return false;
  const std::vector<uint8_t> voters = switch_voters(r);
  if (voters.empty()) return false;
  for (uint8_t v : voters) if (!(r.switch_votes & (1u << v))) return false;
  return true;
}

void apply_switch(Room& r) {
  const std::vector<uint8_t> v = switch_voters(r);
  r.switch_pending = false; r.switch_votes = 0;
  clear_choices(r);
  r.play_winner_next = false;
  r.streak_id = kNone; r.streak = 0;
  if (!r.doubles) {
    if (v.size() != 4) return;
    // The two fighters lead the teams; the next two join them in order.
    for (uint8_t id : v) remove_from_line(r, id);
    r.ring[0] = v[0]; r.ring[1] = v[2]; r.ring[2] = v[1]; r.ring[3] = v[3];
    r.doubles = true;
  } else {
    // One player from each team stays; their partners go to the front of the line.
    const uint8_t a2 = r.ring[1], b2 = r.ring[3];
    r.ring[1] = r.ring[2];
    r.ring[2] = r.ring[3] = kNone;
    r.line.insert(r.line.begin(), b2);
    r.line.insert(r.line.begin(), a2);
    r.doubles = false;
  }
  for (uint16_t& t : r.tenure) t = 0;
}

// The losers of `winner_team` go out and the front of the line comes in, one slot at a time; a slot
// stays when nobody is waiting.
void rotate_losers(Room& r, uint8_t winner_team) {
  const int n = ring_size(r);
  for (int s = 0; s < n; ++s) {
    if (team_of_slot(r, s) == (int)winner_team || r.line.empty()) continue;
    const uint8_t in = pop_front(r);
    r.line.push_back(r.ring[s]);
    r.ring[s] = in;
    r.tenure[s] = 0;
  }
}

// Take turns: whoever has been in longest steps out (a team in doubles); in a tie, the losers.
void rotate_longest(Room& r, uint8_t winner_team) {
  const int teams = 2;
  uint16_t longest[2] = {0, 0};
  for (int s = 0; s < ring_size(r); ++s)
    longest[team_of_slot(r, s)] = std::max(longest[team_of_slot(r, s)], r.tenure[s]);
  int out = longest[0] > longest[1] ? 0 : longest[1] > longest[0] ? 1 : (winner_team == 0 ? 1 : 0);
  (void)teams;
  rotate_losers(r, (uint8_t)(out == 0 ? 1 : 0));   // "the winner" here is the team that stays
}

void bump(Room& r) { ++r.rev; }

// A fighter's spot changes hands: the one asking (or the front of the line) comes in, the fighter
// goes to the back of the line (the front, and plays the winner next, when they asked to).
void apply_leaving(Room& r) {
  const uint8_t s = r.leaving_slot;
  if (s == kNone || s >= ring_size(r)) { r.leaving_slot = kNone; return; }
  const uint8_t leaver = r.ring[s];
  uint8_t in = r.incoming;
  if (in != kNone && role(r, in) == Role::Fighter) in = kNone;
  if (in == kNone) in = pop_front(r); else remove_from_line(r, in);
  if (in != kNone) {
    r.ring[s] = in;
    r.tenure[s] = 0;
    if (r.streak_id == leaver) { r.streak_id = kNone; r.streak = 0; }
    if (r.leaver_plays_winner) { r.line.insert(r.line.begin(), leaver); r.play_winner_next = true; }
    else r.line.push_back(leaver);
  }
  r.leaving_slot = kNone; r.incoming = kNone; r.leaver_plays_winner = false;
}

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
void put_str(std::vector<uint8_t>& o, const std::string& s) { o.push_back((uint8_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }

struct Reader {
  const uint8_t* p; size_t n, at = 0; bool ok = true;
  uint8_t u8() { if (at + 1 > n) { ok = false; return 0; } return p[at++]; }
  uint16_t u16() { if (at + 2 > n) { ok = false; return 0; } uint16_t v = (uint16_t)(p[at] << 8 | p[at + 1]); at += 2; return v; }
  uint32_t u32() { if (at + 4 > n) { ok = false; return 0; }
    uint32_t v = (uint32_t)p[at] << 24 | (uint32_t)p[at + 1] << 16 | (uint32_t)p[at + 2] << 8 | p[at + 3]; at += 4; return v; }
  std::string str(size_t max) { const uint8_t len = u8(); if (!ok || len > max || at + len > n) { ok = false; return {}; }
    std::string s((const char*)p + at, len); at += len; return s; }
};

}  // namespace

// ---------------------------------------------------------------- reading
bool set_character(Room& r, uint8_t id, uint8_t character, uint8_t costume) {
  if (character != kNone && character >= 33) return false;   // the external ids (a mod disc has a few more)
  if (costume > 15) return false;
  for (Member& m : r.members) {
    if (m.id != id) continue;
    if (m.character == character && m.costume == costume) return false;
    m.character = character;
    m.costume = costume;
    bump(r);
    return true;
  }
  return false;
}

const Member* find(const Room& r, uint8_t id) {
  for (const Member& m : r.members) if (m.id == id) return &m;
  return nullptr;
}
int ring_size(const Room& r) { return r.doubles ? 4 : 2; }
int ring_slot(const Room& r, uint8_t id) {
  if (id == kNone) return -1;
  for (int s = 0; s < ring_size(r); ++s) if (r.ring[s] == id) return s;
  return -1;
}
int team_of_slot(const Room& r, int slot) { return r.doubles ? slot / 2 : slot; }
int line_place(const Room& r, uint8_t id) {
  for (size_t i = 0; i < r.line.size(); ++i) if (r.line[i] == id) return (int)i;
  return -1;
}
Role role(const Room& r, uint8_t id) {
  if (!find(r, id)) return Role::Absent;
  if (ring_slot(r, id) >= 0) return Role::Fighter;
  if (line_place(r, id) >= 0) return Role::InLine;
  return Role::Watching;
}
uint8_t owner(const Room& r) {
  uint8_t best = kNone;
  for (const Member& m : r.members) if (m.id < best) best = m.id;
  return best;
}
uint8_t next_up(const Room& r) {
  if (r.incoming != kNone) return r.incoming;
  if (r.asker != kNone) return r.asker;
  return r.line.empty() ? kNone : r.line.front();
}
std::vector<uint8_t> switch_voters(const Room& r) {
  std::vector<uint8_t> v;
  if (r.doubles) {
    for (int s = 0; s < 4; ++s) if (r.ring[s] != kNone) v.push_back(r.ring[s]);
    return v.size() == 4 ? v : std::vector<uint8_t>{};
  }
  for (int s = 0; s < 2; ++s) if (r.ring[s] != kNone) v.push_back(r.ring[s]);
  if (v.size() != 2) return {};
  for (uint8_t id : r.line) if (v.size() < 4) v.push_back(id);
  for (const Member& m : r.members)
    if (v.size() < 4 && std::find(v.begin(), v.end(), m.id) == v.end()) v.push_back(m.id);
  return v.size() == 4 ? v : std::vector<uint8_t>{};
}

bool valid(const Room& r) {
  if (r.members.size() > (size_t)kMaxMembers || (uint8_t)r.mode > 2) return false;
  uint32_t seen = 0;
  for (size_t i = 0; i < r.members.size(); ++i) {
    const Member& m = r.members[i];
    if (m.id >= kMaxMembers || (seen & (1u << m.id))) return false;
    seen |= 1u << m.id;
    if (m.code.empty() || m.code.size() > kMaxCode || m.name.size() > kMaxName) return false;
    for (size_t j = 0; j < i; ++j) if (r.members[j].code == m.code) return false;
  }
  uint32_t placed = 0;
  auto place = [&](uint8_t id) {
    if (id >= kMaxMembers || !(seen & (1u << id)) || (placed & (1u << id))) return false;
    placed |= 1u << id;
    return true;
  };
  const int n = ring_size(r);
  for (int s = 0; s < kRingMax; ++s) {
    if (r.ring[s] == kNone) continue;
    if (s >= n || !place(r.ring[s])) return false;
  }
  for (uint8_t id : r.line) if (!place(id)) return false;
  if (r.doubles) {
    if (r.members.size() < 4) return false;
    for (int s = 0; s < 4; ++s) if (r.ring[s] == kNone) return false;
  }
  auto member_or_none = [&](uint8_t id) { return id == kNone || (id < kMaxMembers && (seen & (1u << id))); };
  if (!member_or_none(r.asker) || !member_or_none(r.incoming) || !member_or_none(r.streak_id) || !member_or_none(r.declined))
    return false;
  if (r.asker != kNone && is_fighting(r, r.asker)) return false;
  if (r.incoming != kNone && is_fighting(r, r.incoming)) return false;
  if (r.refused >> n) return false;
  if (r.leaving_slot != kNone && (r.leaving_slot >= n || r.ring[r.leaving_slot] == kNone)) return false;
  if (r.switch_votes & ~seen) return false;
  if (!r.switch_pending && r.switch_votes) return false;
  return true;
}

// ---------------------------------------------------------------- changes
uint8_t join(Room& r, const std::string& code_in, const std::string& name_in) {
  if (r.members.size() >= (size_t)kMaxMembers) return kNone;
  const std::string code = clean(code_in, kMaxCode, true), name = clean(name_in, kMaxName, false);
  if (code.empty()) return kNone;
  for (const Member& m : r.members) if (m.code == code) return kNone;
  uint8_t id = 0;
  while (find(r, id)) ++id;
  Member m;
  m.id = id; m.code = code; m.name = name.empty() ? code : name;
  r.members.push_back(m);
  r.line.push_back(id);
  normalize(r);
  bump(r);
  return id;
}

bool leave(Room& r, uint8_t id) {
  if (!find(r, id)) return false;
  r.members.erase(std::remove_if(r.members.begin(), r.members.end(), [&](const Member& m) { return m.id == id; }), r.members.end());
  const int s = ring_slot(r, id);
  if (s >= 0) { r.ring[s] = kNone; r.tenure[s] = 0; }
  remove_from_line(r, id);
  if (r.asker == id) { r.asker = kNone; r.refused = 0; }
  if (r.incoming == id) r.incoming = kNone;
  if (r.declined == id) r.declined = kNone;
  if (r.streak_id == id) { r.streak_id = kNone; r.streak = 0; }
  r.switch_votes &= (uint16_t)~(1u << id);
  if (r.switch_pending) { r.switch_pending = false; r.switch_votes = 0; }   // the people voting changed
  if (r.members.empty()) { r = Room{r.rev}; r.mode = Mode::WinnerStays; }
  normalize(r);
  bump(r);
  return true;
}

bool set_mode(Room& r, Mode mode) {
  if ((uint8_t)mode > 2 || r.mode == mode) return false;
  r.mode = mode;
  bump(r);
  return true;
}

bool toggle_line(Room& r, uint8_t id) {
  const Role now = role(r, id);
  if (now == Role::Absent || now == Role::Fighter) return false;
  if (now == Role::InLine) {
    remove_from_line(r, id);
    if (r.incoming == id) r.incoming = kNone;
    if (r.asker == id) { r.asker = kNone; r.refused = 0; }
  } else {
    r.line.push_back(id);
  }
  normalize(r);
  bump(r);
  return true;
}

bool ask(Room& r, uint8_t id) {
  const Role now = role(r, id);
  if (now == Role::Absent || now == Role::Fighter) return false;
  if (r.asker != kNone || r.leaving_slot != kNone) return false;   // one change per game
  r.asker = id;
  r.refused = 0;
  if (r.declined == id) r.declined = kNone;
  bump(r);
  return true;
}

bool answer(Room& r, uint8_t fighter, Answer a) {
  const int s = ring_slot(r, fighter);
  if (s < 0 || r.asker == kNone || (r.refused & (1u << s)) || (uint8_t)a > 2) return false;
  if (a == Answer::No) {
    r.refused |= (uint8_t)(1u << s);
    bool all = true;
    for (int k = 0; k < ring_size(r); ++k) if (r.ring[k] != kNone && !(r.refused & (1u << k))) all = false;
    if (all) { r.declined = r.asker; r.asker = kNone; r.refused = 0; }
  } else {
    r.leaving_slot = (uint8_t)s;
    r.incoming = r.asker;
    r.leaver_plays_winner = a == Answer::YesAndPlayWinner;
    r.asker = kNone;
    r.refused = 0;
    if (!r.in_game) { apply_leaving(r); normalize(r); }   // between games: at once, for the next game
  }
  bump(r);
  return true;
}

bool give_up(Room& r, uint8_t fighter, bool play_winner) {
  const int s = ring_slot(r, fighter);
  if (s < 0) return false;
  if (r.leaving_slot == s) {   // again: take it back
    r.leaving_slot = kNone; r.incoming = kNone; r.leaver_plays_winner = false;
    bump(r);
    return true;
  }
  if (r.leaving_slot != kNone) return false;
  uint8_t in = r.asker;
  if (in == kNone && r.line.empty()) return false;   // nobody to take the spot
  r.leaving_slot = (uint8_t)s;
  r.incoming = in;
  r.leaver_plays_winner = play_winner;
  r.asker = kNone; r.refused = 0;
  if (!r.in_game) { apply_leaving(r); normalize(r); }   // between games: at once, for the next game
  bump(r);
  return true;
}

bool propose_switch(Room& r, uint8_t id) {
  if (!find(r, id) || r.switch_pending) return false;
  const std::vector<uint8_t> voters = switch_voters(r);
  if (voters.empty()) return false;
  r.switch_pending = true;
  r.switch_votes = std::find(voters.begin(), voters.end(), id) != voters.end() ? (uint16_t)(1u << id) : 0;
  if (switch_complete(r) && !r.in_game) apply_switch(r);
  bump(r);
  return true;
}

bool vote_switch(Room& r, uint8_t id, bool yes) {
  if (!r.switch_pending) return false;
  const std::vector<uint8_t> voters = switch_voters(r);
  if (std::find(voters.begin(), voters.end(), id) == voters.end()) return false;
  if (!yes) {
    r.switch_pending = false; r.switch_votes = 0;
  } else {
    if (r.switch_votes & (1u << id)) return false;
    r.switch_votes |= (uint16_t)(1u << id);
    if (switch_complete(r) && !r.in_game) apply_switch(r);
  }
  bump(r);
  return true;
}

bool game_start(Room& r) {
  if (r.in_game) return false;
  for (int s = 0; s < ring_size(r); ++s) if (r.ring[s] == kNone) return false;
  r.in_game = true;
  bump(r);
  return true;
}

bool game_end(Room& r, uint8_t winner_team) {
  if (!r.in_game) return false;
  if (winner_team != kNone && winner_team > 1) return false;
  r.in_game = false;
  r.declined = kNone;
  if (switch_complete(r)) {   // everyone agreed during the game
    if (winner_team != kNone) {
      ++r.games;
      for (int s = 0; s < ring_size(r); ++s) {
        Member* m = const_cast<Member*>(find(r, r.ring[s]));
        if (m) (team_of_slot(r, s) == winner_team ? m->wins : m->losses)++;
      }
    }
    apply_switch(r);
    normalize(r);
    bump(r);
    return true;
  }
  if (winner_team == kNone) { bump(r); return true; }   // no contest: the lineup stays as it was
  ++r.games;
  const int n = ring_size(r);
  for (int s = 0; s < n; ++s) {
    Member* m = const_cast<Member*>(find(r, r.ring[s]));
    if (m) (team_of_slot(r, s) == winner_team ? m->wins : m->losses)++;
    ++r.tenure[s];
  }
  const uint8_t leader = r.ring[r.doubles ? winner_team * 2 : winner_team];
  if (r.streak_id == leader) ++r.streak; else { r.streak_id = leader; r.streak = 1; }

  if (r.leaving_slot != kNone) {
    // A fighter gave up their spot during the game: that change replaces the mode's own rotation.
    apply_leaving(r);
  } else if (r.play_winner_next) {
    r.play_winner_next = false;
    rotate_losers(r, winner_team);
  } else if (r.mode == Mode::WinnerStays) {
    rotate_losers(r, winner_team);
  } else if (r.mode == Mode::TakeTurns) {
    rotate_longest(r, winner_team);
  }
  // A streak belongs to whoever is still in the ring.
  if (r.streak_id != kNone && ring_slot(r, r.streak_id) < 0) { r.streak_id = kNone; r.streak = 0; }
  normalize(r);
  bump(r);
  return true;
}

// ---------------------------------------------------------------- wire form
std::vector<uint8_t> encode(const Room& r) {
  std::vector<uint8_t> o;
  o.push_back(kWireVersion);
  put32(o, r.rev);
  o.push_back((uint8_t)r.mode);
  o.push_back((uint8_t)((r.doubles ? 1 : 0) | (r.in_game ? 2 : 0) | (r.leaver_plays_winner ? 4 : 0) |
                        (r.switch_pending ? 8 : 0) | (r.play_winner_next ? 16 : 0)));
  put32(o, r.games);
  for (int s = 0; s < kRingMax; ++s) { o.push_back(r.ring[s]); put16(o, r.tenure[s]); }
  o.push_back(r.streak_id); put16(o, r.streak);
  o.push_back(r.asker); o.push_back(r.refused); o.push_back(r.declined);
  o.push_back(r.leaving_slot); o.push_back(r.incoming);
  put16(o, r.switch_votes);
  o.push_back((uint8_t)r.members.size());
  for (const Member& m : r.members) {
    o.push_back(m.id); put16(o, m.wins); put16(o, m.losses); o.push_back(m.character); o.push_back(m.costume);
    put_str(o, m.code); put_str(o, m.name);
  }
  o.push_back((uint8_t)r.line.size());
  o.insert(o.end(), r.line.begin(), r.line.end());
  return o;
}

bool decode(const uint8_t* data, size_t size, Room& out) {
  if (!data) return false;
  Reader in{data, size};
  if (in.u8() != kWireVersion) return false;
  Room r;
  r.rev = in.u32();
  r.mode = (Mode)in.u8();
  const uint8_t flags = in.u8();
  if (flags >> 5) return false;
  r.doubles = flags & 1; r.in_game = flags & 2; r.leaver_plays_winner = flags & 4;
  r.switch_pending = flags & 8; r.play_winner_next = flags & 16;
  r.games = in.u32();
  for (int s = 0; s < kRingMax; ++s) { r.ring[s] = in.u8(); r.tenure[s] = in.u16(); }
  r.streak_id = in.u8(); r.streak = in.u16();
  r.asker = in.u8(); r.refused = in.u8(); r.declined = in.u8();
  r.leaving_slot = in.u8(); r.incoming = in.u8();
  r.switch_votes = in.u16();
  const uint8_t count = in.u8();
  if (!in.ok || count > kMaxMembers) return false;
  for (uint8_t i = 0; i < count; ++i) {
    Member m;
    m.id = in.u8(); m.wins = in.u16(); m.losses = in.u16(); m.character = in.u8(); m.costume = in.u8();
    m.code = in.str(kMaxCode); m.name = in.str(kMaxName);
    if (!in.ok) return false;
    r.members.push_back(std::move(m));
  }
  const uint8_t waiting = in.u8();
  if (!in.ok || waiting > kMaxMembers || in.at + waiting != size) return false;
  r.line.assign(data + in.at, data + in.at + waiting);
  if (!valid(r)) return false;
  out = std::move(r);
  return true;
}

// ---------------------------------------------------------------- feed and links
std::vector<std::pair<uint8_t, uint8_t>> feed_tree(const Room& r, int fanout) {
  if (fanout < 1) fanout = 1;
  std::vector<std::pair<uint8_t, uint8_t>> edges;
  // Senders by level: the fighters first, then the watchers who already have the feed.
  std::vector<std::vector<uint8_t>> levels(1);
  std::vector<int> load(kMaxMembers, 0);
  for (int s = 0; s < ring_size(r); ++s) if (r.ring[s] != kNone) levels[0].push_back(r.ring[s]);
  if (levels[0].empty()) return edges;
  // Receivers: the line first (the next to play gets the shortest path), then the watchers by join order.
  std::vector<uint8_t> receivers = r.line;
  for (const Member& m : r.members)
    if (role(r, m.id) == Role::Watching) receivers.push_back(m.id);
  for (uint8_t who : receivers) {
    for (size_t level = 0; level < levels.size(); ++level) {
      uint8_t best = kNone;
      for (uint8_t sender : levels[level])
        if (load[sender] < fanout && (best == kNone || load[sender] < load[best])) best = sender;
      if (best == kNone) continue;
      ++load[best];
      edges.push_back({who, best});
      if (level + 1 >= levels.size()) levels.emplace_back();
      levels[level + 1].push_back(who);
      break;
    }
  }
  return edges;
}

int feed_hops(const Room& r, uint8_t id, int fanout) {
  if (!find(r, id)) return -1;
  if (ring_slot(r, id) >= 0) return 0;
  const auto edges = feed_tree(r, fanout);
  int hops = 0;
  for (uint8_t at = id; hops <= kMaxMembers;) {
    auto it = std::find_if(edges.begin(), edges.end(), [&](const std::pair<uint8_t, uint8_t>& e) { return e.first == at; });
    if (it == edges.end()) return ring_slot(r, at) >= 0 ? hops : -1;
    ++hops;
    at = it->second;
    if (ring_slot(r, at) >= 0) return hops;
  }
  return -1;
}

std::vector<std::pair<uint8_t, uint8_t>> wanted_links(const Room& r, int fanout) {
  std::vector<std::pair<uint8_t, uint8_t>> links;
  auto add = [&](uint8_t a, uint8_t b) {
    if (a == kNone || b == kNone || a == b) return;
    links.push_back({std::min(a, b), std::max(a, b)});
  };
  for (int a = 0; a < ring_size(r); ++a)
    for (int b = a + 1; b < ring_size(r); ++b) add(r.ring[a], r.ring[b]);
  for (const auto& e : feed_tree(r, fanout)) add(e.first, e.second);
  std::sort(links.begin(), links.end());
  links.erase(std::unique(links.begin(), links.end()), links.end());
  return links;
}

}  // namespace slippi::arena
