// Arena room rules: every rotation mode, the ask flow, giving up a spot, the doubles vote, rooms of
// up to 16, the live feed's routing, the wire form, and a random-operations test that checks the
// room never breaks its own rules.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_rules.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace slippi::arena;

namespace {
int g_failed = 0, g_checks = 0;
const char* g_test = "";
#define CHECK(x) do { ++g_checks; if (!(x)) { std::printf("  FAIL %s line %d: %s\n", g_test, __LINE__, #x); ++g_failed; } } while (0)

struct Case { const char* name; std::function<void()> body; };
std::vector<Case>& cases() { static std::vector<Case> c; return c; }
struct Add { Add(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Add add_##name(#name, name); static void name()

Room room_of(int n) {
  Room r;
  const char* names[] = {"HERO", "WISE", "PEACE", "NOVA", "ECHO", "FOXY", "ZAP", "MOSS",
                         "RUNE", "BLAZE", "KITE", "LUX", "ONYX", "PIXEL", "QUILL", "TANGO"};
  for (int i = 0; i < n; ++i) {
    char code[16];
    std::snprintf(code, sizeof code, "%s#%d", names[i], 100 + i);
    join(r, code, names[i]);
  }
  return r;
}
// Plays a game won by the fighter (singles) or team (doubles) whose first slot holds `id`.
void win(Room& r, uint8_t id) {
  CHECK(game_start(r));
  const int s = ring_slot(r, id);
  CHECK(s >= 0);
  CHECK(game_end(r, (uint8_t)team_of_slot(r, s)));
}
std::vector<uint8_t> ring_ids(const Room& r) {
  std::vector<uint8_t> v;
  for (int s = 0; s < ring_size(r); ++s) v.push_back(r.ring[s]);
  return v;
}
bool same(const Room& a, const Room& b) { return encode(a) == encode(b); }
void dump(const Room& r) {
  std::printf("    rev %u mode %d doubles %d in_game %d members", r.rev, (int)r.mode, r.doubles, r.in_game);
  for (const Member& m : r.members) std::printf(" %u", m.id);
  std::printf("\n    ring %d %d %d %d line", r.ring[0], r.ring[1], r.ring[2], r.ring[3]);
  for (uint8_t id : r.line) std::printf(" %u", id);
  std::printf("\n    asker %d refused %d declined %d leaving %d incoming %d plays_winner %d next %d switch %d votes %04X streak %d\n",
              r.asker, r.refused, r.declined, r.leaving_slot, r.incoming, r.leaver_plays_winner, r.play_winner_next,
              r.switch_pending, r.switch_votes, r.streak_id);
}
}  // namespace

// ---------------------------------------------------------------- joining and leaving
TEST(join_fills_ring_then_line) {
  Room r = room_of(4);
  CHECK(r.members.size() == 4);
  CHECK(r.ring[0] == 0 && r.ring[1] == 1);
  CHECK((r.line == std::vector<uint8_t>{2, 3}));
  CHECK(role(r, 0) == Role::Fighter && role(r, 2) == Role::InLine && role(r, 9) == Role::Absent);
  CHECK(valid(r));
}
TEST(join_refuses_same_code_any_case) {
  Room r = room_of(2);
  CHECK(join(r, "hero#100", "Someone") == kNone);
  CHECK(join(r, " HERO#100", "Someone") != kNone);   // a different code (leading space) is a different player
  CHECK(r.members.size() == 3);
}
TEST(join_full_at_sixteen) {
  Room r = room_of(16);
  CHECK(r.members.size() == 16 && valid(r));
  CHECK(join(r, "LATE#999", "LATE") == kNone);
  CHECK(r.line.size() == 14);
}
TEST(join_cleans_code_and_name) {
  Room r;
  const uint8_t id = join(r, "ab\x01" "cd#12345678", std::string("Name\n") + std::string(60, 'x'));
  const Member* m = find(r, id);
  CHECK(m && m->code == "ABCD#12345" && m->code.size() == kMaxCode);
  CHECK(m && m->name.size() == kMaxName && m->name.find('\n') == std::string::npos);
  CHECK(join(r, "\x02\x03", "x") == kNone);   // nothing left of the code
}
TEST(ids_reuse_the_lowest_free) {
  Room r = room_of(4);
  CHECK(leave(r, 1));
  CHECK(join(r, "NEW#1", "NEW") == 1);
  CHECK(owner(r) == 0);
  CHECK(leave(r, 0));
  CHECK(owner(r) == 1);
}
TEST(leave_fighter_refills_from_line) {
  Room r = room_of(4);
  CHECK(leave(r, 1));
  CHECK(r.ring[0] == 0 && r.ring[1] == 2);
  CHECK((r.line == std::vector<uint8_t>{3}));
  CHECK(!leave(r, 1));
  CHECK(valid(r));
}
TEST(last_member_leaving_empties_room) {
  Room r = room_of(1);
  CHECK(r.ring[0] == 0 && r.ring[1] == kNone);
  const uint32_t rev = r.rev;
  CHECK(leave(r, 0));
  CHECK(r.members.empty() && r.ring[0] == kNone && r.rev > rev && valid(r));
}

// ---------------------------------------------------------------- Winner stays
TEST(winner_stays_loser_to_back_front_in) {
  Room r = room_of(4);   // 0 v 1, line 2 3
  win(r, 0);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2}));
  CHECK((r.line == std::vector<uint8_t>{3, 1}));
  win(r, 2);
  CHECK((ring_ids(r) == std::vector<uint8_t>{3, 2}));
  CHECK((r.line == std::vector<uint8_t>{1, 0}));
}
TEST(winner_stays_with_nobody_waiting) {
  Room r = room_of(2);
  win(r, 1);
  win(r, 0);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}));
  CHECK(r.line.empty() && r.games == 2);
}
TEST(streak_counts_and_moves) {
  Room r = room_of(4);
  win(r, 0); win(r, 0);
  CHECK(r.streak_id == 0 && r.streak == 2);
  win(r, 3);   // 0 v 3 (2 lost the second game); 3 wins
  CHECK(r.streak_id == 3 && r.streak == 1);
}
TEST(record_wins_and_losses) {
  Room r = room_of(3);
  win(r, 0); win(r, 0); win(r, 1);
  CHECK(find(r, 0)->wins == 2 && find(r, 0)->losses == 1);
  CHECK(find(r, 2)->losses == 1 && find(r, 1)->wins == 1 && find(r, 1)->losses == 1);
}
TEST(eight_member_rotation_everyone_plays) {
  Room r = room_of(8);
  std::set<uint8_t> played;
  for (int g = 0; g < 7; ++g) { for (uint8_t id : ring_ids(r)) played.insert(id); win(r, r.ring[g % 2]); }
  for (uint8_t id : ring_ids(r)) played.insert(id);
  CHECK(played.size() == 8);
  CHECK(r.games == 7 && valid(r));
}

// ---------------------------------------------------------------- Take turns
TEST(take_turns_first_game_loser_out) {
  Room r = room_of(3);
  CHECK(set_mode(r, Mode::TakeTurns));
  win(r, 0);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2}));
  CHECK((r.line == std::vector<uint8_t>{1}));
}
TEST(take_turns_veteran_out_even_after_winning) {
  Room r = room_of(3);
  set_mode(r, Mode::TakeTurns);
  win(r, 0);   // 1 out
  win(r, 0);   // 0 has two games in, 2 one: 0 steps out although 0 won
  CHECK((ring_ids(r) == std::vector<uint8_t>{1, 2}));
  CHECK((r.line == std::vector<uint8_t>{0}));
}
TEST(take_turns_is_fair_over_many_games) {
  Room r = room_of(3);
  set_mode(r, Mode::TakeTurns);
  std::map<uint8_t, int> games;
  for (int g = 0; g < 30; ++g) { for (uint8_t id : ring_ids(r)) ++games[id]; win(r, r.ring[0]); }
  CHECK(games[0] >= 19 && games[0] <= 21);
  CHECK(games[1] >= 19 && games[1] <= 21);
  CHECK(games[2] >= 19 && games[2] <= 21);
}

// ---------------------------------------------------------------- Ask
TEST(ask_mode_nothing_moves_on_its_own) {
  Room r = room_of(3);
  set_mode(r, Mode::Ask);
  win(r, 1); win(r, 0);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}));
  CHECK((r.line == std::vector<uint8_t>{2}));
}
TEST(ask_yes_takes_that_fighters_spot) {
  Room r = room_of(3);
  set_mode(r, Mode::Ask);
  CHECK(ask(r, 2));
  CHECK(next_up(r) == 2);
  CHECK(answer(r, 0, Answer::Yes));   // between games: at once, for the next game
  CHECK(r.leaving_slot == kNone && r.asker == kNone);
  CHECK((ring_ids(r) == std::vector<uint8_t>{2, 1}));
  CHECK((r.line == std::vector<uint8_t>{0}));
}
TEST(ask_answered_during_a_game_waits_for_its_end) {
  Room r = room_of(3);
  set_mode(r, Mode::Ask);
  CHECK(game_start(r));
  CHECK(ask(r, 2));
  CHECK(answer(r, 0, Answer::Yes));
  CHECK(r.leaving_slot == 0 && r.incoming == 2);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}));   // the game in progress keeps its players
  CHECK(game_end(r, 0));                                 // the result does not matter: the spot was given
  CHECK((ring_ids(r) == std::vector<uint8_t>{2, 1}));
  CHECK((r.line == std::vector<uint8_t>{0}));
}
TEST(ask_yes_and_i_play_the_winner) {
  Room r = room_of(4);
  set_mode(r, Mode::Ask);
  CHECK(ask(r, 3));
  CHECK(answer(r, 1, Answer::YesAndPlayWinner));   // 1 lets 3 in now and goes to the front of the line
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 3}));
  CHECK((r.line == std::vector<uint8_t>{1, 2}) && r.play_winner_next);
  win(r, 3);   // 1 plays the winner (3); 0 lost and goes to the back, even in Ask mode
  CHECK((ring_ids(r) == std::vector<uint8_t>{1, 3}));
  CHECK((r.line == std::vector<uint8_t>{2, 0}) && !r.play_winner_next);
}
TEST(ask_declined_by_both) {
  Room r = room_of(3);
  CHECK(ask(r, 2));
  CHECK(answer(r, 0, Answer::No));
  CHECK(r.asker == 2 && r.declined == kNone);
  CHECK(!answer(r, 0, Answer::No));   // one answer each
  CHECK(answer(r, 1, Answer::No));
  CHECK(r.asker == kNone && r.declined == 2);
  CHECK(game_start(r) && game_end(r, 0) && r.declined == kNone);
}
TEST(ask_one_no_then_other_yes) {
  Room r = room_of(3);
  set_mode(r, Mode::Ask);
  ask(r, 2);
  CHECK(answer(r, 0, Answer::No));
  CHECK(answer(r, 1, Answer::Yes));
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2}) && (r.line == std::vector<uint8_t>{1}));
}
TEST(ask_refused_cases) {
  Room r = room_of(4);
  CHECK(!ask(r, 0));        // a fighter cannot ask
  CHECK(!ask(r, 9));        // not in the room
  CHECK(ask(r, 3));
  CHECK(!ask(r, 2));        // one ask at a time
  CHECK(!answer(r, 2, Answer::Yes));   // only fighters answer
}
TEST(ask_from_a_watcher_jumps_the_line) {
  Room r = room_of(4);
  set_mode(r, Mode::Ask);
  CHECK(toggle_line(r, 3));   // 3 just watches
  CHECK(role(r, 3) == Role::Watching);
  CHECK(ask(r, 3) && answer(r, 1, Answer::Yes));
  CHECK(r.ring[1] == 3 && (r.line == std::vector<uint8_t>{2, 1}));
}

TEST(an_asker_rotated_in_is_no_longer_asking) {
  Room r = room_of(3);
  set_mode(r, Mode::TakeTurns);
  CHECK(ask(r, 2));
  win(r, 0);   // Take turns brings 2 in for 1 anyway
  CHECK(r.ring[1] == 2 && r.asker == kNone && valid(r));
}

// ---------------------------------------------------------------- giving up a spot
TEST(give_up_and_take_it_back) {
  Room r = room_of(3);
  CHECK(game_start(r));       // during a game the change waits for its end, and can be taken back
  CHECK(give_up(r, 1));
  CHECK(r.leaving_slot == 1 && next_up(r) == 2);
  CHECK(give_up(r, 1));       // again: taken back
  CHECK(r.leaving_slot == kNone);
  CHECK(give_up(r, 0));
  CHECK(!give_up(r, 1));      // one change per game
}
TEST(give_up_between_games_is_at_once) {
  Room r = room_of(3);
  CHECK(give_up(r, 1));
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2}) && (r.line == std::vector<uint8_t>{1}));
  CHECK(r.leaving_slot == kNone);
}
TEST(give_up_needs_someone_to_come_in) {
  Room r = room_of(2);
  CHECK(!give_up(r, 0));
  Room r3 = room_of(3);
  toggle_line(r3, 2);         // the only other member just watches
  CHECK(!give_up(r3, 0));
}
TEST(give_up_replaces_the_mode_rotation) {
  Room r = room_of(4);        // Winner stays
  CHECK(game_start(r));
  CHECK(give_up(r, 0));       // during the game, 0 lets the front of the line in
  CHECK(game_end(r, 0));      // 0 even wins: the voluntary change happens instead of 1 going out
  CHECK((ring_ids(r) == std::vector<uint8_t>{2, 1}));
  CHECK((r.line == std::vector<uint8_t>{3, 0}));
  CHECK(r.streak_id == kNone);   // the streak left with 0
}
TEST(give_up_uses_a_waiting_ask) {
  Room r = room_of(4);
  ask(r, 3);
  CHECK(give_up(r, 1));
  CHECK(r.ring[1] == 3 && r.asker == kNone);
}

// ---------------------------------------------------------------- watching and the line
TEST(toggle_line_watch_and_back) {
  Room r = room_of(4);
  CHECK(toggle_line(r, 2));
  CHECK(role(r, 2) == Role::Watching && (r.line == std::vector<uint8_t>{3}));
  CHECK(toggle_line(r, 2));
  CHECK((r.line == std::vector<uint8_t>{3, 2}));
  CHECK(!toggle_line(r, 0));   // a fighter does not leave the ring this way
  CHECK(!toggle_line(r, 7));
}
TEST(watchers_are_skipped_by_rotation) {
  Room r = room_of(4);
  toggle_line(r, 2);
  win(r, 0);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 3}) && role(r, 2) == Role::Watching);
}

// ---------------------------------------------------------------- games
TEST(no_contest_keeps_the_lineup) {
  Room r = room_of(4);
  CHECK(game_start(r));
  give_up(r, 1);
  CHECK(game_end(r, kNone));
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}) && r.games == 0);
  CHECK(r.leaving_slot == 1);   // still decided for the next game
  CHECK(find(r, 0)->wins == 0 && find(r, 1)->losses == 0);
  CHECK(game_start(r) && game_end(r, 0));
  CHECK(r.ring[1] == 2);
}
TEST(game_start_and_end_order) {
  Room r = room_of(1);
  CHECK(!game_start(r));       // nobody to fight
  CHECK(!game_end(r, 0));
  Room r2 = room_of(2);
  CHECK(game_start(r2) && !game_start(r2));
  CHECK(!game_end(r2, 2));     // no team 2
  CHECK(game_end(r2, 1) && !game_end(r2, 1));
}
TEST(rev_counts_only_changes) {
  Room r = room_of(2);
  const uint32_t rev = r.rev;
  CHECK(!set_mode(r, Mode::WinnerStays) && r.rev == rev);
  CHECK(set_mode(r, Mode::TakeTurns) && r.rev == rev + 1);
  CHECK(!set_mode(r, (Mode)7) && r.rev == rev + 1);
}

// ---------------------------------------------------------------- doubles
TEST(doubles_needs_four) {
  Room r = room_of(3);
  CHECK(switch_voters(r).empty());
  CHECK(!propose_switch(r, 0));
}
TEST(doubles_everyone_says_yes) {
  Room r = room_of(5);    // 0 v 1, line 2 3 4
  CHECK((switch_voters(r) == std::vector<uint8_t>{0, 1, 2, 3}));
  CHECK(propose_switch(r, 2));
  CHECK(r.switch_pending && !r.doubles);
  CHECK(!vote_switch(r, 4, true));   // 4 would not play
  CHECK(vote_switch(r, 0, true) && vote_switch(r, 1, true));
  CHECK(!r.doubles);
  CHECK(vote_switch(r, 3, true));
  CHECK(r.doubles && !r.switch_pending && valid(r));
  // Teams: 0 with 2, 1 with 3.
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2, 1, 3}));
  CHECK((r.line == std::vector<uint8_t>{4}));
}
TEST(doubles_one_no_cancels) {
  Room r = room_of(4);
  propose_switch(r, 0);
  CHECK(vote_switch(r, 1, true));
  CHECK(vote_switch(r, 3, false));
  CHECK(!r.switch_pending && r.switch_votes == 0 && !r.doubles);
}
TEST(doubles_vote_during_a_game_waits_for_its_end) {
  Room r = room_of(4);
  CHECK(game_start(r));
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  CHECK(!r.doubles && r.switch_pending);
  CHECK(game_end(r, 0));
  CHECK(r.doubles && find(r, 0)->wins == 1 && find(r, 1)->losses == 1);
}
TEST(doubles_winner_stays_swaps_the_losing_team) {
  Room r = room_of(6);
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2, 1, 3}));   // line 4 5
  win(r, 0);   // team 0+2 wins; 1 and 3 go out, 4 and 5 come in
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 2, 4, 5}));
  CHECK((r.line == std::vector<uint8_t>{1, 3}));
  CHECK(find(r, 2)->wins == 1 && find(r, 3)->losses == 1);
  CHECK(r.streak_id == 0 && r.streak == 1);
}
TEST(doubles_back_to_singles) {
  Room r = room_of(4);
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  CHECK(r.doubles);
  CHECK((switch_voters(r) == std::vector<uint8_t>{0, 2, 1, 3}));
  propose_switch(r, 3); vote_switch(r, 0, true); vote_switch(r, 1, true); vote_switch(r, 2, true);
  CHECK(!r.doubles && valid(r));
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}));   // one from each team stays
  CHECK((r.line == std::vector<uint8_t>{2, 3}));
}
TEST(doubles_falls_back_when_a_player_leaves) {
  Room r = room_of(4);
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  CHECK(leave(r, 2));   // nobody waiting to take the spot
  CHECK(!r.doubles && valid(r));
  CHECK((ring_ids(r) == std::vector<uint8_t>{0, 1}) && (r.line == std::vector<uint8_t>{3}));
}
TEST(doubles_refills_from_the_line) {
  Room r = room_of(5);
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  CHECK(leave(r, 3));
  CHECK(r.doubles && r.ring[3] == 4 && r.line.empty());
}

// ---------------------------------------------------------------- wire form
TEST(wire_round_trip) {
  Room r = room_of(9);
  win(r, 0); win(r, 2);   // Winner stays: 2 came in for 1
  set_mode(r, Mode::Ask);
  toggle_line(r, 6);
  ask(r, 6); answer(r, 0, Answer::No);
  std::vector<uint8_t> bytes = encode(r);
  Room back;
  CHECK(decode(bytes.data(), bytes.size(), back));
  CHECK(same(r, back) && back.members[3].name == r.members[3].name);
  Room d = room_of(6);
  propose_switch(d, 0); vote_switch(d, 1, true); vote_switch(d, 2, true); vote_switch(d, 3, true);
  bytes = encode(d);
  CHECK(decode(bytes.data(), bytes.size(), back) && back.doubles && same(d, back));
}
TEST(character_picks_travel_with_the_room) {
  Room r = room_of(3);
  CHECK(r.members[1].character == kNone);
  const uint32_t rev = r.rev;
  CHECK(set_character(r, 1, 8, 2));              // Mario, third costume
  CHECK(r.rev == rev + 1);
  CHECK(!set_character(r, 1, 8, 2));             // the same pick again changes nothing
  CHECK(!set_character(r, 1, 200, 0));           // no such character
  CHECK(!set_character(r, 9, 8, 0));             // no such member
  CHECK(set_character(r, 1, kNone, 0));          // picked nothing again
  set_character(r, 2, 25, 4);
  const std::vector<uint8_t> bytes = encode(r);
  Room back;
  CHECK(decode(bytes.data(), bytes.size(), back));
  CHECK(back.members[2].character == 25 && back.members[2].costume == 4 && back.members[1].character == kNone);
}
TEST(wire_refuses_every_truncation_and_garbage) {
  Room r = room_of(5);
  const std::vector<uint8_t> bytes = encode(r);
  Room out;
  for (size_t n = 0; n < bytes.size(); ++n) CHECK(!decode(bytes.data(), n, out));
  std::vector<uint8_t> longer = bytes;
  longer.push_back(0);
  CHECK(!decode(longer.data(), longer.size(), out));
  std::vector<uint8_t> version = bytes;
  version[0] = 9;
  CHECK(!decode(version.data(), version.size(), out));
  CHECK(!decode(nullptr, 10, out));
}
TEST(wire_refuses_an_impossible_room) {
  Room r = room_of(4);
  std::vector<uint8_t> bytes = encode(r);
  // The line's last id becomes a fighter's: the same member twice.
  bytes.back() = r.ring[0];
  Room out;
  CHECK(!decode(bytes.data(), bytes.size(), out));
  Room bad = room_of(3);
  bad.asker = bad.ring[0];   // a fighter asking
  bytes = encode(bad);
  CHECK(!decode(bytes.data(), bytes.size(), out));
}

// ---------------------------------------------------------------- live feed and links
TEST(feed_small_room) {
  Room r = room_of(3);
  const auto tree = feed_tree(r);
  CHECK(tree.size() == 1 && tree[0].first == 2 && tree[0].second == 0);
  CHECK(feed_hops(r, 2) == 1 && feed_hops(r, 0) == 0 && feed_hops(r, 7) == -1);
}
TEST(feed_sixteen_fanout_and_hops) {
  Room r = room_of(16);
  toggle_line(r, 9);   // one watcher among the line
  const auto tree = feed_tree(r, 3);
  CHECK(tree.size() == 14);
  std::map<uint8_t, int> load;
  for (const auto& e : tree) ++load[e.second];
  for (const auto& l : load) CHECK(l.second <= 3);
  int worst = 0;
  for (const Member& m : r.members) if (role(r, m.id) != Role::Fighter) worst = std::max(worst, feed_hops(r, m.id, 3));
  CHECK(worst <= 3);
  CHECK(feed_hops(r, r.line.front(), 3) == 1);   // next up gets it straight from a fighter
  // Fanout 1 still reaches everyone, through a chain.
  CHECK(feed_tree(r, 1).size() == 14);
}
TEST(links_join_up_the_whole_room) {
  for (int n : {2, 3, 4, 8, 16}) {
    Room r = room_of(n);
    const auto links = wanted_links(r);
    CHECK(std::is_sorted(links.begin(), links.end()));
    std::set<uint8_t> reached = {r.ring[0]};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto& l : links) {
        const bool a = reached.count(l.first), b = reached.count(l.second);
        if (a != b) { reached.insert(a ? l.second : l.first); grew = true; }
      }
    }
    CHECK((int)reached.size() == n);
    CHECK(std::find(links.begin(), links.end(), std::make_pair(std::min(r.ring[0], r.ring[1]), std::max(r.ring[0], r.ring[1]))) != links.end());
    CHECK((int)links.size() <= n);   // a tree plus the fighters' own link
  }
}
TEST(links_in_doubles_connect_all_four_players) {
  Room r = room_of(6);
  propose_switch(r, 0); vote_switch(r, 1, true); vote_switch(r, 2, true); vote_switch(r, 3, true);
  const auto links = wanted_links(r);
  int fighter_pairs = 0;
  for (const auto& l : links) if (ring_slot(r, l.first) >= 0 && ring_slot(r, l.second) >= 0) ++fighter_pairs;
  CHECK(fighter_pairs == 6);   // every pair of the four plays over its own link, as in Teams
}

// ---------------------------------------------------------------- random operations
TEST(random_operations_keep_the_room_valid) {
  const int failed_before = g_failed;
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(seed);
    Room r;
    int next_code = 0;
    for (int step = 0; step < 600; ++step) {
      const uint32_t before = r.rev;
      const std::vector<uint8_t> was = encode(r);
      const uint8_t id = (uint8_t)(rng() % 18);
      bool changed = false;
      const uint32_t op = rng() % 12;
      Room prior = r;
      switch (op) {
        case 0: { char code[16]; std::snprintf(code, sizeof code, "P%d#1", next_code++); changed = join(r, code, "P") != kNone; break; }
        case 1: changed = (rng() % 4 == 0) && leave(r, id); break;
        case 2: changed = set_mode(r, (Mode)(rng() % 3)); break;
        case 3: changed = toggle_line(r, id); break;
        case 4: changed = ask(r, id); break;
        case 5: changed = answer(r, id, (Answer)(rng() % 3)); break;
        case 6: changed = give_up(r, id, rng() % 2); break;
        case 7: changed = propose_switch(r, id); break;
        case 8: changed = vote_switch(r, id, rng() % 5 != 0); break;
        case 9: case 10: changed = game_start(r); break;
        default: changed = game_end(r, (uint8_t)(rng() % 3 == 0 ? kNone : rng() % 2)); break;
      }
      CHECK(valid(r));
      CHECK(changed ? r.rev > before : (r.rev == before && encode(r) == was));
      const std::vector<uint8_t> bytes = encode(r);
      Room back;
      CHECK(decode(bytes.data(), bytes.size(), back) && encode(back) == bytes);
      if (g_failed != failed_before) {
        std::printf("  seed %u step %d op %u id %u\n  before:\n", seed, step, op, id);
        dump(prior);
        std::printf("  after:\n");
        dump(r);
        return;
      }
    }
  }
}

int main() {
  int passed = 0;
  for (const Case& c : cases()) {
    g_test = c.name;
    const int before = g_failed;
    c.body();
    if (g_failed == before) ++passed;
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("arena_rules_test: %d of %zu tests passed (%d checks)\n", passed, cases().size(), g_checks);
  return g_failed ? 1 : 0;
}
