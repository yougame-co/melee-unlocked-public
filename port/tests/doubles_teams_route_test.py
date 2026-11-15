"""YouGame Doubles on the 4-peer Teams client: the room's teams, the stage rule and the route's wiring."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('fixture', ROOT / 'tools/native_major_fixture.py')
fixture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fixture)


def read(path):
    return (ROOT / path).read_text()


class DoublesTeamsRules(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix='doubles-teams-') as directory:
            cpp = Path(directory) / 'test.cpp'
            exe = Path(directory) / 'test'
            cpp.write_text(source)
            subprocess.run(['c++', '-std=c++17', '-I' + str(ROOT / 'port/runtime/hle'),
                            '-I' + str(ROOT / 'port/third_party'), str(cpp), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_room_seats_stage_and_refusal(self):
        out = self.compile_run(r'''
#include "yougame_doubles_teams.h"
#include <cassert>
#include <cstdio>
using namespace yougame::doubles_teams;
static json seat(int slot, const std::string& c, int team = -2) {
  json s = {{"id", "p" + c}, {"connectionId", c}, {"localIndex", 0}, {"slot", slot}};
  if (team != -2) s["team"] = team;
  return s;
}
static json lobby(json seats, json seed = "00000000000000ff") {
  return {{"t", "lobby"}, {"nativeTeams", {{"v", 1}, {"kind", "match"}, {"muDoubles", true}, {"selectionEpoch", 7},
          {"dissolved", false}, {"seats", seats}, {"seed", seed}}}};
}
int main() {
  static_assert(kFourPeer, "the 4-peer route is the default");
  assert(match_search(3, true) && !match_search(3, false) && !match_search(1, true));
  // Seats are matched by their own slot, never by array position.
  Seats s;
  assert(read(lobby({seat(2, "c", 1), seat(0, "a", 0), seat(3, "d", 1), seat(1, "b", 0)}), s));
  assert(s.known && s.connection[0] == "a" && s.connection[2] == "c" && s.team[2] == 1 && s.epoch == 7);
  assert(s.seed == "00000000000000ff");
  // A room may cross the pairing; it owns the teams.
  assert(read(lobby({seat(0, "a", 0), seat(1, "b", 1), seat(2, "c", 0), seat(3, "d", 1)}), s));
  assert(plan(Setup{}, s, "matchaaa").team[1] == 1 && plan(Setup{}, s, "matchaaa").team[2] == 0);
  // No team named: slots 0,1 team 0; slots 2,3 team 1.
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}), s));
  assert(s.team[0] == 0 && s.team[1] == 0 && s.team[2] == 1 && s.team[3] == 1);
  // Nested under room, as a welcome carries it.
  Seats nested;
  json welcome = {{"t", "welcome"}, {"room", lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")})["nativeTeams"]}};
  welcome["room"] = {{"nativeTeams", welcome["room"]}};
  assert(read(welcome, nested) && nested.known);
  // Anything malformed leaves the seats as they were.
  Seats keep = s;
  const json bad[] = {
    lobby({seat(0, "a"), seat(1, "b"), seat(2, "c")}),                               // three seats
    lobby({seat(0, "a"), seat(0, "b"), seat(2, "c"), seat(3, "d")}),                 // a slot twice
    lobby({seat(0, "a"), seat(1, "a"), seat(2, "c"), seat(3, "d")}),                 // a peer twice
    lobby({seat(0, "a", 0), seat(1, "b", 0), seat(2, "c", 0), seat(3, "d", 1)}),     // three a side
    lobby({seat(0, "a", 2), seat(1, "b"), seat(2, "c"), seat(3, "d")}),              // no such team
    lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(4, "d")}),                 // no such slot
    lobby({seat(0, ""), seat(1, "b"), seat(2, "c"), seat(3, "d")}),                  // no peer
  };
  for (const auto& m : bad) { Seats t = keep; assert(!read(m, t)); assert(t.connection == keep.connection); }
  json friends = lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")});
  friends["nativeTeams"]["muDoubles"] = false; assert(!read(friends, keep));
  json home = lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")});
  home["nativeTeams"]["kind"] = "home"; assert(!read(home, keep));
  assert(!read(json{{"t", "lobby"}, {"nativeTeams", {{"activeRoom", "matchaaa"}}}}, keep));
  // A room placed before the seed existed: the seats count, the seed comes from the room code.
  Seats old;
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}, nullptr), old) && old.seed.empty());
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}, "00000000000000FF"), old) && old.seed.empty());
  assert(seed_value("", "matchaaa") == fnv64("mu-doubles:matchaaa"));
  assert(seed_value("", "matchaaa") != seed_value("", "matchaab"));
  assert(seed_value("00000000000000ff", "matchaaa") == 255);
  // The roster agrees with the seats by connection id.
  json roster = json::array({{{"connectionId", "a"}, {"slot", 0}}, {{"connectionId", "b"}, {"slot", 1}},
                             {{"connectionId", "c"}, {"slot", 2}}, {{"connectionId", "d"}, {"slot", 3}}});
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}), s));
  assert(consistent(s, roster));
  roster[1]["slot"] = 2; roster[2]["slot"] = 1; assert(!consistent(s, roster));
  assert(consistent(Seats{}, json()));
  // The local player's team from the queue's matched frame.
  assert(matched_team({{"t", "matched"}, {"team", 1}}) == 1);
  assert(matched_team({{"t", "matched"}, {"slot", 2}}) == 1);
  assert(matched_team({{"t", "matched"}, {"slot", 1}}) == 0);
  assert(matched_team({{"t", "matched"}, {"room", "matchaaa"}}) == -1);
  // Stages: always doubles-legal (never Fountain of Dreams, 2), all five reachable, the same for the same
  // inputs. The seed and the game index only: no epoch, nothing a peer chooses.
  std::set<int> first, later;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    for (uint32_t game = 0; game < 6; ++game) {
      const int id = stage(seed, game);
      assert(id == 31 || id == 32 || id == 28 || id == 8 || id == 3);
      (game ? later : first).insert(id);
      assert(stage(seed, game) == id);
    }
    assert(stage(seed, 0) == kStages[seed % 5]);
  }
  assert(first.size() == 5 && later.size() == 5);
  int moved = 0;
  for (uint64_t seed = 0; seed < 200; ++seed) moved += stage(seed, 1) != stage(seed, 2);
  assert(moved > 100);
  // The start: Rooms' console session at the host's begin names the match, its seed and every seat.
  const std::string digest = "mu-doubles:v1:test";
  auto part = [](int port, const std::string& c, int team) {
    return json{{"slot", port - 1}, {"port", port}, {"id", "p" + c}, {"connectionId", c}, {"user", "u" + c},
                {"name", c}, {"team", team}};
  };
  auto start = [&](json parts) {
    return json{{"t", "start"}, {"matchId", "sess1"}, {"console", {{"sessionId", "sess1"}, {"seed", "00000000000000a1"},
      {"startedAt", 1}, {"inputDelay", 3}, {"settingsDigest", digest}, {"teamBattle", true}, {"participants", parts}}}};
  };
  const json crossed = json::array({part(3, "c", 0), part(1, "a", 0), part(2, "b", 1), part(4, "d", 1)});
  Setup setup;
  assert(read_start(start(crossed), digest, setup) == nullptr);
  assert(setup.known && setup.match == "sess1" && setup.seed == "00000000000000a1");
  assert(setup.team[0] == 0 && setup.team[1] == 1 && setup.team[2] == 0 && setup.team[3] == 1);
  assert(setup.id[2] == "pc" && setup.connection[3] == "d");
  assert(seated(setup, "b") && !seated(setup, "e") && !seated(Setup{}, "a"));
  // Rooms' start also carries a top-level seed, a uint32 number with another value, and the room's own
  // participants: the stage seed, ports and teams come from the console session only.
  { json m = start(crossed); m["seed"] = 3735928559u;
    m["participants"] = json::array({part(1, "d", 1), part(2, "c", 1), part(3, "b", 0), part(4, "a", 0)});
    Setup top; assert(read_start(m, digest, top) == nullptr);
    assert(top.seed == "00000000000000a1" && top.team == setup.team && top.connection == setup.connection);
    Seats seats; assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}), seats));
    assert(plan(top, seats, "matchaaa").seed == 0xa1); }
  json in_room = {{"t", "welcome"}, {"room", {{"console", start(crossed)["console"]}}}};
  Setup nested_setup; assert(read_start(in_room, digest, nested_setup) == nullptr && nested_setup.known);
  // Anything else is refused with a reason, and leaves the setup as it was.
  std::vector<json> refused;
  { json m = start(crossed); m["matchId"] = "other"; refused.push_back(m); }
  { json m = start(crossed); m["console"]["seed"] = "A1"; refused.push_back(m); }
  { json m = start(crossed); m["console"]["teamBattle"] = false; refused.push_back(m); }
  { json m = start(crossed); m["console"]["settingsDigest"] = "other"; refused.push_back(m); }
  { json m = start(crossed); m["console"]["sessionId"] = ""; refused.push_back(m); }
  { json m = start(crossed); m["console"]["participants"].erase(0); refused.push_back(m); }
  refused.push_back(start(json::array({part(1, "a", 0), part(2, "b", 0), part(3, "c", 0), part(4, "d", 1)})));  // three a side
  refused.push_back(start(json::array({part(1, "a", 0), part(1, "b", 0), part(3, "c", 1), part(4, "d", 1)})));  // a port twice
  refused.push_back(start(json::array({part(1, "a", 0), part(2, "a", 0), part(3, "c", 1), part(4, "d", 1)})));  // a peer twice
  refused.push_back(start(json::array({part(1, "a", 0), part(2, "b", 0), part(3, "c", 1), part(5, "d", 1)})));  // no such port
  { json bad_slot = part(2, "b", 0); bad_slot["slot"] = 3;
    refused.push_back(start(json::array({part(1, "a", 0), bad_slot, part(3, "c", 1), part(4, "d", 1)}))); }
  { json m = start(crossed); m["console"]["participants"][0]["team"] = 2; refused.push_back(m); }
  refused.push_back(json{{"t", "start"}, {"matchId", "sess1"}});                                                // no session
  for (const auto& m : refused) { Setup t = setup; assert(read_start(m, digest, t) != nullptr); assert(t.match == "sess1" && t.team == setup.team); }
  // The plan: the start outranks the lobby's seats and seed.
  Seats lobby_seats;
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}), lobby_seats));
  Plan from_start = plan(setup, lobby_seats, "matchaaa");
  assert(!from_start.refusal && from_start.team == setup.team && from_start.seed == 0xa1);
  assert(from_start.seats.known && from_start.seats.connection[3] == "d");
  // Without a start the room's seats and seed; a room from before the seed takes its code, seats known.
  Plan from_room = plan(Setup{}, lobby_seats, "matchaaa");
  assert(!from_room.refusal && from_room.team[0] == 0 && from_room.team[2] == 1 && from_room.seed == 0xff);
  Seats before_seed;
  assert(read(lobby({seat(0, "a"), seat(1, "b"), seat(2, "c"), seat(3, "d")}, nullptr), before_seed));
  assert(plan(Setup{}, before_seed, "matchaaa").seed == fnv64("mu-doubles:matchaaa"));
  // Seats unknown: refused on screen, never slot order, never a code seed.
  Plan unknown = plan(Setup{}, Seats{}, "matchaaa");
  assert(unknown.refusal == kSeatsUnknown && unknown.team[0] == -1 && unknown.seed == 0);
  // The room owns the pairs: two a side, each selected player sending a colour (red 0, blue 1, green 2).
  // Its 0x82 team_id is that colour now, not its room team: any colour plays, and never moves a team.
  const std::array<int, 4> room{{0, 0, 1, 1}};
  assert(team_conflict(room, room, 0xF) == nullptr);
  assert(team_conflict(room, {{0, 1, 1, 1}}, 0xF) == nullptr);         // partners on two colours: still a pair
  assert(team_conflict(room, {{2, 2, 2, 2}}, 0xF) == nullptr);         // everyone green: the clash rule settles it
  assert(team_conflict(room, {{0, 3, 1, 1}}, 0xF) == kTeamMismatch);   // no such colour
  assert(team_conflict(room, {{0, 0, -1, 1}}, 0xF) == kTeamMismatch);
  assert(team_conflict(room, room, 0x7) == kTeamMismatch);             // three in the match
  assert(team_conflict({{0, 0, 0, 1}}, {{0, 0, 0, 1}}, 0xF) == kTeamMismatch);
  assert(team_conflict({{0, -1, 1, 1}}, {{0, -1, 1, 1}}, 0xF) == kTeamMismatch);
  assert(team_conflict({{1, 0, 1, 0}}, {{0, 0, 0, 0}}, 0xF) == nullptr); // pairs by the room, whatever the slots
  // A team wears its lowest slot's colour, team 1 off a clash; all four hold the same 0x82s, so agree.
  for (int c0 = 0; c0 < kTeamColors; ++c0)
    for (int c1 = 0; c1 < kTeamColors; ++c1)
      for (int other = 0; other < kTeamColors; ++other) {
        const auto color = team_colors(room, {{c0, other, c1, other}}, 0xF);
        assert(color[0] == c0 && color[0] != color[1]);
        assert(color[1] == (c1 != c0 ? c1 : c0 == 0 ? 1 : 0));
        const auto swapped = team_colors({{1, 0, 0, 1}}, {{c1, c0, other, other}}, 0xF);
        assert(swapped[0] == c0 && swapped[1] == (c1 != c0 ? c1 : c0 == 0 ? 1 : 0));
      }
  assert((team_colors(room, {{-1, 9, -1, 9}}, 0xF) == std::array<int, 2>{{0, 1}}));   // none sent: red, blue
  assert((team_colors(room, {{2, 2, 2, 2}}, 0x5) == std::array<int, 2>{{2, 0}}));     // only present slots count
  std::puts("ok");
}
''')
        self.assertEqual(out.strip(), 'ok')


MENU_STUBS = r'''
#include <cassert>
#include <cstdio>
using u8 = unsigned char;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT, MU_SLP_MODE_TEAMS, MU_SLP_MODE_PARTY,
       MU_SLP_OPT_LOGIN, MU_SLP_OPT_LOGOUT, MU_SLP_OPT_UPDATE, MU_SLP_OPT_FRIENDS, MU_SLP_OPT_SLIPPI,
       MU_SLP_OPT_TOURNAMENT, MU_SLP_OPT_DOUBLES };
#include "mu_online_menu.h"
enum { MENU_KIND_8 = 8, GM_BOOT = 0, GM_VS = 2, GM_HANYU_CSS = 8, SPLIT_FIRST_SLIPPI = 3, YG_OPEN_DIRECT = 2 };
#define MU_OPTION_SLIPPI_MENUS 0x8u
static unsigned options;                 // the host's game options: 0 inside a console session
unsigned mu_game_options() { return options; }
static int reports;
void OSReport(const char*, ...) { ++reports; }
static struct { int mode, app_state; } slp;
static int agent_lock_logged, friends_css_host, tournament_visible, slippi_submenu, ranked_supported, friends_return, doubles_home_return,
  boot_oneshot, boot_main_menu, split_page, last_slot = -1, yg_open, auto_slot, p2p_auto_direct, installs, content = 99;
void mu_mnmain_install_online_menu() { ++installs; }
void mu_content_mode(int mode) { content = mode; }
static int split_layout() { return 0; }
static int split_start_page() { return 0; }
static int cursor_stops(int) { return 1; }
int mu_slippi_first_unlocked() { return 0; }
int mn_80229938(int, int) { return 1; }
int mu_slippi_fetch_app_state() { return 1; }
int mu_online_p2p_menus() { return 0; }
void mu_slippi_load_match_state(void*) {}
static int direct_row() { return 0; }
enum { CMD_YOUGAME_FRIENDS_MENU = 0xE6 };
// The host's one-shot "a friend room opened the CSS" query (14): never, in a Doubles session.
int mu_slippi_cmd(unsigned cmd, const void* p, unsigned, void* out, unsigned, unsigned* got) {
  if (cmd == CMD_YOUGAME_FRIENDS_MENU && *(const u8*)p == 14) { *(u8*)out = 0; *got = 1; return 0; }
  return -1;
}
'''


class DoublesMenuReturn(unittest.TestCase):
    """The game's own menu entry after a Doubles session: Online with Doubles highlighted."""

    def test_results_and_b_land_on_online_doubles(self):
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        doubles = read('sourceport/game/shim/mu_doubles.c')
        production = '\n'.join(fixture.extract(menu, name) for name in
                               ('mu_slippi_menus_enabled', 'menus_relatch', 'mu_slippi_menu_enter'))
        teams = '\n'.join(fixture.extract(doubles, name) for name in
                          ('mu_doubles_teams_enter', 'mu_doubles_take_teams_return', 'mu_doubles_take_menu_return'))
        source = MENU_STUBS + 'static int menus_flag = -1;\nstatic int menu_return, teams_css, teams_team = -1;\n' + \
            teams + production + r'''
int main() {
  u8 kind = 0xEE, hovered = 0xEE;
  const int doubles_row = mu_online_menu_row(1, 0, MU_SLP_OPT_DOUBLES);
  // The console path: the session boots from before main, where the host's options are all 0, and
  // latches the online menus off (mu_slippi_boot_mode is the first to ask).
  options = 0; assert(!mu_slippi_menus_enabled());
  // Its results (or B out of its character select) leave the borrowed VS mode: the Doubles return is set.
  menu_return = 1;
  // Without the relatch the entry is retail's: VS. Mode with Melee highlighted, the reported bug.
  assert(!mu_slippi_menu_enter(GM_VS, &kind, &hovered) && kind == 0xEE && menu_return == 1);
  // A menu entry while the session still runs keeps the canonical menus (the same on all four).
  menus_relatch(); assert(!mu_slippi_menus_enabled());
  // Once the session is over the host's menus come back on the next main-menu entry (menu prep).
  options = MU_OPTION_SLIPPI_MENUS; menus_relatch(); assert(mu_slippi_menus_enabled());
  // A menu without the Doubles row (bit 2 of the host's FA reply clear) leaves the return pending.
  ranked_supported = 0;
  assert(!mu_slippi_menu_enter(GM_VS, &kind, &hovered) && menu_return == 1);
  ranked_supported = 1;
  assert(mu_slippi_menu_enter(GM_VS, &kind, &hovered));
  assert(kind == MENU_KIND_8 && hovered == doubles_row && menu_return == 0 && !slippi_submenu);
  // Taken once: the next entry from VS is retail's again.
  kind = hovered = 0xEE; assert(!mu_slippi_menu_enter(GM_VS, &kind, &hovered) && kind == 0xEE);
  // The relatch never turns the menus off.
  options = 0; menus_relatch(); assert(mu_slippi_menus_enabled());
  // A menu without the Doubles row leaves the Teams return pending as well.
  mu_doubles_teams_enter(1); slp.mode = MU_SLP_MODE_TEAMS; content = 99; ranked_supported = 0;
  mu_slippi_menu_enter(GM_HANYU_CSS, &kind, &hovered);
  assert(teams_css == 1 && teams_team == 1);
  ranked_supported = 1;
  // The 4-peer path: B out of the home's Teams character select comes back the same way, offline view again.
  mu_doubles_teams_enter(1); slp.mode = MU_SLP_MODE_TEAMS; content = 99;
  assert(mu_slippi_menu_enter(GM_HANYU_CSS, &kind, &hovered));
  assert(kind == MENU_KIND_8 && hovered == doubles_row && content == -1 && !slippi_submenu);
  assert(!teams_css && teams_team == -1);
  // An ordinary Teams character select goes back to its Slippi row.
  assert(mu_slippi_menu_enter(GM_HANYU_CSS, &kind, &hovered) && slippi_submenu &&
         hovered == mu_online_menu_row(1, 1, MU_SLP_MODE_TEAMS));
  // B out of the Doubles home (the held lobby) lands on Online with Doubles highlighted, not the Slippi submenu.
  doubles_home_return = 1; friends_return = 1; slippi_submenu = 1; kind = hovered = 0xEE;
  assert(mu_slippi_menu_enter(GM_HANYU_CSS, &kind, &hovered));
  assert(kind == MENU_KIND_8 && hovered == doubles_row && !slippi_submenu && !friends_return && !doubles_home_return);
  // Without the Doubles row it is dropped, never left to fire on a later entry.
  doubles_home_return = 1; ranked_supported = 0; mu_slippi_menu_enter(GM_HANYU_CSS, &kind, &hovered);
  assert(!doubles_home_return); ranked_supported = 1;
  std::puts("ok");
}
'''
        with tempfile.TemporaryDirectory(prefix='doubles-menu-') as directory:
            cpp = Path(directory) / 'test.cpp'
            exe = Path(directory) / 'test'
            cpp.write_text(source)
            subprocess.run(['c++', '-std=c++17', '-DMELEE_DOUBLES=1', '-Wno-unused-function',
                            '-I' + str(ROOT / 'sourceport/game/include'), str(cpp), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        self.assertEqual(out.strip(), 'ok')

    def test_menu_prep_relatches_before_it_reads_the_flag(self):
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        prep = fixture.extract(menu, 'mu_slippi_menu_prep')
        self.assertLess(prep.index('menus_relatch();'), prep.index('if (!mu_slippi_menus_enabled())'))


class DoublesTeamsWiring(unittest.TestCase):
    def test_find_match_joins_the_home_match_room(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('doubles_teams::match_search(search.mode, !search.friend_room.empty())', link)
        self.assertIn('if (doubles_match) request = doubles::ticket(compat, team_room, 4);', link)
        self.assertIn('if (team_code && !doubles_match && !friends::team_room_code(', link)
        self.assertIn('if (doubles_match) if (auto ready = consent.ready(full)) room->socket->send(*ready);', link)
        room_fn = link[link.index('DoublesRoom doubles_room() {'):]
        room_fn = room_fn[:room_fn.index('\n}\n')]
        self.assertIn('const auto plan = doubles_teams::plan(g_room->setup, g_room->doubles, g_room->code);', room_fn)
        self.assertIn('if (plan.refusal) return out;', room_fn)
        self.assertIn('out.consistent = doubles_teams::consistent(plan.seats, g_room->participants);', room_fn)
        self.assertNotIn('seed_value', room_fn)
        # Ready, then the host's console begin, then the start's match: the loop waits for it.
        self.assertIn('room->socket->send(consent.begin(3));', link)
        self.assertIn('room->me == room->host_connection && consent.have_epoch', link)
        self.assertIn('doubles_teams::read_start(m, doubles::digest(), started)', link)
        self.assertIn('setup = started; room->match_id = setup.match;', link)
        self.assertIn('if (all && (!doubles_match || setup.known)) break;', link)
        self.assertIn('return fail("The Doubles match didn\'t start", true);', link)
        self.assertLess(link.index('setup = started; room->match_id = setup.match;'),
                        link.index('if (all && (!doubles_match || setup.known)) break;'))
        # Transient failures search the room again, kMaxTries in all.
        self.assertIn('if (doubles::ui.retry_teams(g_doubles_search_room))', link)
        self.assertIn('m.retry = false; why = doubles::kTeamsCappedLine;', link)
        self.assertIn('if (g_room->setup.known && g_room->settled) return;', link)
        self.assertIn('io.settled = [code = r.code] { doubles::ui.settle_teams(code); };', link)
        room = read('port/runtime/hle/yougame_room.h')
        self.assertIn('if (r.size == 4) doubles_teams::read(m, r.doubles);', room)
        self.assertIn('if (setup.known) return setup.team[player_idx];', room)
        self.assertIn('if (room_closed && settled) io.log("yougame: the Doubles room closed after its match (a normal end)");', room)

    def test_home_sends_each_seat_to_its_own_character_select(self):
        runtime = read('port/runtime/hle/yougame_doubles_runtime.inc')
        branch = runtime[runtime.index('#if MU_DOUBLES_FOUR_PEER\n        if (admission)'):]
        branch = branch[:branch.index('#endif')]
        self.assertIn('teams_route=true;', branch)
        self.assertIn('ui.teams_room=room; ui.teams_team=doubles_teams::matched_team(m); ui.teams_enter=true;', branch)
        self.assertNotIn('ConsoleBackend', branch)
        self.assertIn('ui.playing=true; ui.foreground=false;', branch)
        self.assertIn('h.compatibility=compatibility(doubles_teams::kTeamsMode);', runtime)
        self.assertIn('auto in_match=[&] { return match || preparation.valid() || teams_route; };', runtime)
        self.assertNotIn('match || preparation.valid())', runtime.replace(
            'return match || preparation.valid() || teams_route;', ''))

    def test_search_query_and_stage_in_the_slippi_client(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        find = online[online.index('// The Doubles CSS\'s search (mu_slippi_css.c)'):]
        find = find[:find.index('#endif')]
        self.assertIn('yougame::doubles::ui.teams_search_room()', find)
        self.assertIn('uint8_t find[19] = {};', find)
        self.assertIn('g_doubles_games = 0;', find)
        self.assertIn('start_find_match(find, room); return true;', find)
        self.assertIn('else if (search.mode == Matchmaking::TEAMS && friend_room.empty()) g_teams_codes->AddOrUpdateCode(', online)
        self.assertIn('yougame::doubles::ui.take_teams_entry(idle)', online)
        self.assertIn('const bool idle = yougame::active() && session_mode() < 0;', online)
        self.assertIn('yougame::doubles::ui.teams_over()', find)
        stage = online.index('stage_id = yougame::doubles_teams::stage(doubles_room.seed, g_doubles_games);')
        frame1 = online[online.index('if (doubles_route()) {\n        ++g_doubles_games;'):]
        frame1 = frame1[:frame1.index('#endif')]
        # The first frame of each game tells the home a game of its match began on this seat.
        self.assertIn('yougame::doubles::ui.teams_game();', frame1)
        refusal = online[online.index('const char* refusal = !doubles_room.open'):]
        refusal = refusal[:refusal.index('} else')]
        self.assertIn(': doubles_room.refusal ? doubles_room.refusal', refusal)
        self.assertIn('yougame::doubles_teams::team_conflict(room_team, sent, present)', refusal)
        self.assertIn('g_forced_error = refusal;', refusal)
        self.assertIn('cleanup_connection();', refusal)
        self.assertIn('room_teams_known = true;', refusal)
        self.assertGreater(online.index('const char* refusal'), stage)
        # The colours (11-11 report): the toggle's team_id goes out as the colour, the room's pairs wear
        # team_colors in the block, and the result names the room's teams, never the colours.
        self.assertNotIn('doubles_seat_team', online)
        lock = online[online.index('bool lock_room_team_selection() {'):]
        lock = lock[:lock.index('\n}\n')]
        self.assertLess(lock.index('if (doubles_route()) return false;'), lock.index('g_local_selections.team_id = (uint8_t)team;'))
        self.assertIn('team_color = yougame::doubles_teams::team_colors(room_team, sent, present);', refusal)
        self.assertIn('if (doubles) team = (uint8_t)team_color[room_teams[s->player_idx]];', online)
        # After the whole room/all-same chain, never between its if and else.
        self.assertIn('if (room_teams_known) team = (uint8_t)room_teams[s->player_idx];\n'
                      '      else if (teams && all_same_team) team = team_assign[s->player_idx];\n#ifdef MELEE_DOUBLES\n'
                      '      if (doubles) team = (uint8_t)team_color[room_teams[s->player_idx]];', online)
        self.assertIn('if (doubles) for (int i = 0; i < 4; ++i) g_game_facts.teams[i] = room_teams[i];', online)

    def test_menu_and_character_select(self):
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        block = menu[menu.index('u8 teams_query = 14'):]
        block = block[:block.index('return;')]
        self.assertIn('mu_doubles_teams_enter(teams[1]);', block)
        self.assertIn('slp.mode = MU_SLP_MODE_TEAMS;', block)
        self.assertIn('data->pending_mode = GM_HANYU_CSS;', block)
        self.assertIn('previous_mode == GM_HANYU_CSS && ranked_supported && mu_doubles_take_teams_return()', menu)
        css = read('sourceport/game/shim/mu_slippi_css.c')
        self.assertIn('return mu_slippi_state()->mode == MU_SLP_MODE_TEAMS && mu_doubles_teams_css();', css)
        self.assertIn('tx_lock_in(SB_RAND);', css)
        search = css[css.index('if (!dt.doubles_searched'):]
        search = search[:search.index('return 0;')]
        self.assertIn('mu_slippi_sss_find_match();', search)
        self.assertIn('st->team_idx = (u8) (mu_doubles_teams_team() + 1);', css)
        self.assertIn('if (doubles_teams()) {\n        return;', css)
        shim = read('sourceport/game/shim/mu_doubles.c')
        self.assertIn('int mu_doubles_take_teams_return(void)', shim)

    def test_a_leave_before_the_first_game_searches_again(self):
        # The room's start reaches the character select before anyone picks, so a dissolve there is a
        # failed match (the home comes back and searches), not one in progress.
        runtime = read('port/runtime/hle/yougame_doubles_runtime.inc')
        branch = runtime[runtime.index('else if (text(m,"t")=="teams-dissolved" && teams_route)'):]
        branch = branch[:branch.index('else if (text(m,"t")=="teams-dissolved")')]
        self.assertIn('played=ui.teams_played;', branch)
        self.assertIn('else if (!entered || !started || !played) {', branch)
        self.assertIn('back(); teams_failure(reason,text(m,"by"));', branch)
        self.assertIn('"no game yet"', branch)
        failure = runtime[runtime.index('auto teams_failure=[&]'):]
        failure = failure[:failure.index('\n  };')]
        self.assertIn('if (!by.empty() && by==h.me)', failure)
        self.assertIn('const bool partner=!by.empty() && match_seats.count(by);', failure)
        self.assertIn('"Your partner left"', failure)
        self.assertIn('"A player didn\'t connect"', failure)
        self.assertIn('"A player left"', failure)
        self.assertIn('h.notice=requeue_wait_line(h.requeue_why); wait_notice=true;', failure)
        self.assertIn('if (after_failure(std::string(),by) && h.phase==Phase::Home) {', failure)
        # The partner's leave promises no search until Rooms drops them from the home.
        self.assertIn('h.notice=kPartnerLeftNotice;', failure)
        # The partner's leave: the seats left search as a new party once Rooms drops them.
        partner = runtime[runtime.index('auto requeue_after_partner=[&]'):]
        partner = partner[:partner.index('\n  };')]
        self.assertIn('if (!now.count(requeue_left) && h.is_host()) {', partner)
        self.assertIn('start_seats=now; start_host=true; requeues=0; h.retry=0; pending_requeue=true;', partner)
        self.assertIn('kPartnerLeftMatchLine', partner)
        # Three automatic searches per START: only a game that began gives the select new tries.
        self.assertIn('if (ui.teams_played) { requeues=0; h.retry=0; }', runtime)
        branch4 = runtime[runtime.index('host::log("doubles: match found, the four players pick on their own character select");'):]
        branch4 = branch4[:branch4.index('} else')]
        self.assertNotIn('requeues=0', branch4)
        self.assertIn('wait_notice=false; requeue_left.clear(); h.requeue_why.clear(); return had;', runtime)
        self.assertIn('match_seats=seats();', runtime)
        self.assertIn('      requeue_after_partner();\n      requeue();', runtime)
        header = read('port/runtime/hle/yougame_doubles.h')
        self.assertIn('if (!requeue_why.empty()) line=requeue_why+" - searching again: "', header)
        self.assertIn('return why+" - searching again in a moment\\nB: leave";', header)
        self.assertIn('teams_started=teams_settled=teams_played=false;', header)
        # The select never offers a code to enter on this route: it says what it waits for.
        css = read('sourceport/game/shim/mu_slippi_css.c')
        self.assertIn('int doubles_waiting = doubles_teams() && conn < MU_SLP_MM_CONNECTION_SUCCESS;', css)
        self.assertIn('"Connecting to players"', css)
        self.assertIn('"Match closed. Hold B to exit"', css)
        self.assertLess(css.index('if (doubles_waiting && m->local_ready == 0) {'), css.index('action = "enter code";'))

    def test_home_returns_on_the_main_menu(self):
        boot = read('port/app/doubles_boot.inc')
        self.assertIn('slippi::online::native_cleanup_match();', boot)
        self.assertIn('if(seen_online && scene[0]==1 && !left) { left=true; doubles::ui.command("match-ended"); }', boot)
        # back() clears playing before the stop is served: the stop still ends this seat's Teams session.
        self.assertIn('teams_room=doubles::ui.playing || doubles::ui.stop_match?doubles::ui.teams_room:std::string();', boot)
        self.assertIn('if(leave_teams_css && leave_css("the home is back")) leave_teams_css=false;', boot)
        self.assertIn('if(teams_settled && !left && !settled_out && scene[0]==8) settled_out=leave_css("the match is over");', boot)


class DoublesLobbyColours(unittest.TestCase):
    """The Doubles lobby match (frozen setup): each machine's 0x82 team_id is its colour (11-11 report)."""

    def test_colours_come_from_the_selections_as_sent(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        # Taken from the machines' own selections before the frozen picks replace them in `ordered`.
        fill = online.index('if (lps.player_idx < PLAYER_COUNT_MAX) lobby_colors[lps.player_idx] = lps.team_id;')
        self.assertLess(online.index('std::array<int, PLAYER_COUNT_MAX> lobby_colors;'), online.index('if (local_ready && remote_ready) {'))
        self.assertIn('lobby_colors[rps[i].player_idx] = rps[i].team_id;', online)
        self.assertLess(fill, online.index('ordered[port] = &pick;'))
        built = online[online.index('if (setup_built && lobby_plan) {'):]
        self.assertLess(built.index('const auto team_colors = yougame::lobby::doubles_colors(*lobby_plan, lobby_colors);'),
                        built.index('yougame::lobby::apply(*lobby_plan, block.data(), block.size(), timer, g_lobby_costumes.data(), &team_colors);'))
        # The setup hash reads the block (colours in it); the inputs stay the match id, so a peer that mapped
        # other colours is a MISMATCH within a second, never a wait to the connect timeout.
        self.assertIn('setup_inputs = yougame::lobby::inputs_hash(*lobby_plan);', built)
        self.assertIn('if (lobby_plan->doubles)', built)
        self.assertIn('g_game_facts.teams[i] = i < lobby_plan->ports ? lobby_plan->port[i].pick.team : -1;', built)
        # The result is Rooms' teams: finish_report names pick.team, never the colours.
        setup = read('port/runtime/hle/yougame_lobby_setup.h')
        self.assertIn('r["detail"] = {{"tw", p.port[order[0]].pick.team}}', setup)
        compat = read('port/runtime/hle/yougame_compatibility.h')
        self.assertIn('inline constexpr int kOnlineProtocol = 2;', compat)


if __name__ == '__main__':
    unittest.main()
