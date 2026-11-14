"""Friends: local menus, picks as messages, and Squad Strike lineups in a friend room.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def body(source, name):
    at = source.index(name)
    at = source.index('{', at)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end]


class FriendsLocalMenus(unittest.TestCase):
    def test_policy_keeps_shared_menus_off_for_friends(self):
        policy = read('port/runtime/hle/' + 'yougame_mode_policy.h')
        self.assertIn('constexpr bool friends_shared_menus = false;', policy)
        self.assertIn('#include "yougame_mode_policy.h"', read('port/runtime/hle/native_practice.cpp'))

    def test_friend_room_starts_known_peer_rollback_as_direct_winners(self):
        tick = body(read('port/runtime/hle/native_practice.cpp'), 'void tick()')
        local = tick[tick.index('if (yougame::mode_policy::friends_shared_menus && g_bridge) {'):]
        local = local[local.index('} else {'):]
        order = ['set_online_mode(2);',
                 'g_bridge(OP_DIRECT_FIRST_MATCH, a, 1);',
                 'slippi::online::native_start_match(2, {}, 0, 0, &error, room);',
                 'request_major(kOnlineMajor);']
        at = [local.index(step) for step in order]
        self.assertEqual(at, sorted(at))
        self.assertIn('kFirstCssWinner = 1', read('port/runtime/hle/' + 'yougame_friends.h'))

    def test_rematch_stays_in_the_room(self):
        # Direct's winner/loser flow runs the next game on the same connection; the private
        # Friends room is kept open by rooms after each result, and only Unranked asks to stay.
        link = read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('if (search.mode == 1) request["afterMatch"] = "stay";', link)
        # The 4-player Friends lobby (yougame_lobby.h) is its own route, never the 1v1 friend room.
        self.assertIn('const bool friends_mode = !team_code && !lobby_match && (!search.friend_room.empty());', link)


class FriendsSquad(unittest.TestCase):
    def test_friend_room_search_needs_no_lineup_or_budget(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        find = online[online.index('void start_find_match('):]
        find = find[:find.index('g_last_search = search;')]
        self.assertIn('search.squad = yougame::squad::selected(search.mode, g_squad_row, false);', find)
        self.assertIn('if (search.squad && friend_room.empty()) {', find)
        # A friend room plays Squad Strike only when "Play a friend"'s own picker returned it: never
        # the row and never a leftover choice. The intent is spent on that one search.
        own = find[find.index('} else {'):]
        own = own[:own.index('\n  }')]
        for step in ['search.squad = search.mode == Matchmaking::DIRECT && friend_room == g_squad_friend_room;',
                     'g_squad_friend_room.clear();', 'g_squad_friend_intent = false;']:
            self.assertIn(step, own)
        # Each lock-in in a friend room hands the lineup over when the room is Squad Strike.
        sel = body(online, 'void set_match_selections(')
        self.assertIn('const bool squad = friend_room ? (g_last_search.squad || (g_netplay && g_netplay->SquadAdopted()))', sel)
        self.assertIn(': yougame::squad::selected(payload[7], g_squad_row, false);', sel)

    def test_friend_lock_in_held_until_the_friends_build(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        sel = body(online, 'void set_match_selections(')
        hold = sel.index('if (friend_room && g_netplay && s.is_character_selected && g_netplay->SquadFriendWaiting()) {')
        # Before the merge: the host never counts (or sends) the lock-in while it waits.
        self.assertLess(hold, sel.index('g_local_selections.Merge(s);'))
        self.assertIn('host::log("squad: lock-in held, waiting for the friend\'s game");', sel[hold:])
        header = read('port/runtime/hle/slippi_net.h')
        self.assertIn('bool SquadFriendWaiting() const { return squad_friend_room_ && !squad_peer_build_.load() && !squad_search_.load(); }', header)

    def test_picker_room_is_the_only_squad_room(self):
        tick = body(read('port/runtime/hle/native_practice.cpp'), 'void tick()')
        self.assertIn('try { room = g_friend_picker.get(); picked = !room.empty(); }', tick)
        join = tick[tick.index('if (!room.empty()) {\n        slippi::online::squad_friend_room(room, picked);'):]
        self.assertLess(join.index('squad_friend_room(room, picked);'), join.index('native_start_match(2, {}, 0, 0, &error, room);'))
        # Every way the picker ends without a room turns Squad Strike off.
        self.assertGreaterEqual(tick.count('slippi::online::squad_friend_picker_closed();'), 5)
        online = read('port/runtime/hle/slippi_online.cpp')
        room = body(online, 'void squad_friend_room(const std::string& room, bool picked) {')
        self.assertIn('if (picked && g_squad_friend_intent) {', room)
        self.assertIn('host::log("squad: another friend room was joined, Squad Strike off");', room)
        closed = body(online, 'void squad_friend_picker_closed() {')
        self.assertIn('host::log("squad: friend picker closed without a room, Squad Strike off");', closed)

    def test_lineup_rides_with_lock_in_only(self):
        net = read('port/runtime/hle/slippi_net.cpp')
        send = body(net, 'void NetplayClient::SetMatchSelections(')
        self.assertIn('if (squad_advertised_.load() && match_info_.local.is_character_selected) {', send)
        self.assertLess(send.index('SendAsync(std::move(lineup));'), send.index('WriteSelections(*spac, match_info_.local);'))
        receive = net[net.index('case NP_MSG_SLIPPI_MATCH_SELECTIONS: {'):]
        receive = receive[:receive.index('case NP_MSG_SLIPPI_CHAT_MESSAGE:')]
        self.assertIn('if ((squad_search_ || squad_peer_cap_.load()) && s->is_character_selected) {', receive)

    def test_friend_ticket_and_key_stay_plain(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('const std::string compat = compatibility(search.mode, squad_row, search.squad_budget);', link)
        self.assertIn('request["mode"] = squad::ticket_mode(search.mode, true, "singles");', link)
        squad = read('port/runtime/hle/yougame_squad.h')
        for helper in ['ticket_mode', 'pool_tag', 'caps']:
            self.assertNotIn('selected(mode, row, true)', body(squad, helper))

    def test_selected_and_keys_compile(self):
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('needs a C++ compiler')
        program = r'''
#include "yougame_squad.h"
#include <cassert>
#include <cstdio>
int main() {
  using namespace yougame::squad;
  static_assert(selected(1, true) && !selected(2, true) && !selected(1, true, true));
  static_assert(selected(2, true, true) && !selected(2, false, true) && !selected(0, true, true));
  // Two friends with different rows share one ticket and key, so they meet and see why on screen.
  const std::string budget(64, 'a');
  for (bool row : {false, true}) {
    assert(compatibility("0.9.96", false, "", false, {}, "", 2, row, "") ==
           compatibility("0.9.96", false, "", false, {}, "", 2, false, ""));
    assert(std::string(ticket_mode(2, row, "singles")) == "singles");
    assert(caps(0, 2, row) == 0);
  }
  assert(compatibility("0.9.96", false, "", false, {}, "", 1, true, "").empty());
  assert(compatibility("0.9.96", false, "", false, {}, "", 1, true, budget).find("squad") != std::string::npos);
  puts("friends squad policy: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix='friends-squad-') as directory:
            work = Path(directory)
            source = work / 'policy.cpp'
            source.write_text(program)
            exe = work / 'policy'
            subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                            '-I', str(ROOT / 'port/runtime/hle'), '-I', str(ROOT / 'port/third_party'),
                            str(source), '-o', str(exe)], check=True, capture_output=True, text=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
            self.assertIn('PASS', out)



class FriendsSquadConsoleSession(unittest.TestCase):
    """A Friends console session from a squad build: its own key, a per-session costume rule, floors."""

    def test_console_key_separates_squad_builds(self):
        io = body(read('port/runtime/hle/yougame_link.cpp'), 'SessionIO console_io(')
        start = io.index('#ifdef MELEE_PORT_VERSION')
        squad = io.index('#if MU_SQUAD', start)
        end = io.index('#endif', squad) + len('#endif')
        self.assertIn('io.compatibility += "-" + yougame::squad::build_tag("");', io[squad:end])
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('needs a C++ compiler')
        program = ('#include "yougame_squad.h"\n#include "yougame_compatibility.h"\n#include <cstdio>\n'
                   '#include <regex>\n#include <string>\n'
                   'using namespace yougame;\nstruct { std::string compatibility; } io;\nint main() {\n' + io[start:end] +
                   '\n  if (!std::regex_match(io.compatibility, std::regex("^[a-zA-Z0-9._:-]{1,64}$"))) return 1;\n'
                   '  std::puts(io.compatibility.c_str());\n}\n')
        keys = {}
        with tempfile.TemporaryDirectory(prefix='friends-console-key-') as directory:
            work = Path(directory)
            (work / 'key.cpp').write_text(program)
            for squad_on in (0, 1):
                for version in ('-DMELEE_PORT_VERSION="0.9.105-online-test.66"', '-DUNUSED_VERSION=1'):
                    exe = work / f'key{squad_on}'
                    build = subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-DMU_SQUAD={squad_on}',
                                            version, '-I', str(ROOT / 'port/runtime/hle'),
                                            str(work / 'key.cpp'), '-o', str(exe)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    keys[squad_on, version] = subprocess.run([str(exe)], check=True, capture_output=True,
                                                             text=True).stdout.strip()
        for version in ('-DMELEE_PORT_VERSION="0.9.105-online-test.66"', '-DUNUSED_VERSION=1'):
            plain, squad_key = keys[0, version], keys[1, version]
            self.assertTrue(plain.endswith('-console5'), plain)     # plain builds: byte-identical
            self.assertEqual(squad_key, plain + '-sq')
        self.assertIn('0.9.105-online-test.66', keys[1, '-DMELEE_PORT_VERSION="0.9.105-online-test.66"'])

    def test_costume_rule_forgotten_at_session_edges_and_host_reset(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        forget = body(online, 'static void squad_console_forget()')
        for line in ['g_squad_console.off();', 'g_squad_console_over.clear();', 'g_squad_fallback_logged.clear();']:
            self.assertIn(line, forget)
        self.assertIn('void squad_console_reset() { g_squad_console_reset.store(true, std::memory_order_release); }',
                      online)
        rule = body(online, 'bool squad_costume_rule(')
        self.assertLess(rule.index('squad_console_forget();'), rule.index('g_squad_console.rule('))
        op = online[online.index('case MU_SQUAD_OP_CONSOLE: {'):]
        self.assertLess(op.index('squad_console_forget();'), op.index('MU_SQUAD_CONSOLE_FACT'))
        engine = read('port/runtime/hle/yougame_console_engine.cpp')
        self.assertIn('if (hooks_.session_edge) hooks_.session_edge();\n  return true;', body(engine, 'bool Engine::begin('))
        self.assertIn('if (hooks_.session_edge) hooks_.session_edge();', body(engine, 'void Engine::end('))
        host = read('port/app/source_host.cpp')
        self.assertEqual(host.count('hooks.session_edge = slippi::online::squad_console_reset;'), 2)
        self.assertEqual(host.count('EngineHooks hooks;'), 2)
        reset = host[host.index('services.reset_host = [&] {'):]
        reset = reset[:reset.index('\n    };')]
        self.assertIn('#if MU_SQUAD\n      slippi::online::squad_console_reset();', reset)

    def test_friends_squads_check_this_machines_heaps(self):
        budget = body(read('sourceport/game/shim/mu_squad.c'), 'bool gmSquad_Budget(')
        self.assertIn('if (mu_console_active() && (s_allm_budget < MU_SQUAD_ONLINE_ALLM || '
                      's_alla_budget < MU_SQUAD_ONLINE_ALLA)) {', budget)
        css = read('sourceport/game/shim/mu_squad_css.c')
        self.assertIn('if (gmSquad_Any() || squad_icons_tried || gmSquad_OnlineChosen() || squad_any_note()) '
                      'squad_banners();', css)


class FriendsSquadPlayAFriend(unittest.TestCase):
    """Play a friend on the Squad Strike row: the friend room plays Squad Strike, never a 1v1."""

    def test_friend_op_and_cap_are_bounded(self):
        wire = read('port/runtime/abi/mu_squad_wire.h')
        self.assertIn('#define MU_SQUAD_FRIEND_CAP 0x80', wire)
        self.assertIn('#define MU_SQUAD_OP_FRIEND 11', wire)
        bridge = read('port/runtime/hle/native_slippi_bridge.h')
        self.assertIn('case MU_SQUAD_OP_FRIEND: return payload_len == 2 && payload[1] <= MU_SQUAD_FRIEND_INTENT;', bridge)
        for define in ['#define MU_SQUAD_FRIEND_QUERY 0', '#define MU_SQUAD_FRIEND_INVITE 1',
                       '#define MU_SQUAD_FRIEND_REFUSE 2', '#define MU_SQUAD_FRIEND_INTENT 3',
                       '#define MU_SQUAD_ROOM_PLAIN 0', '#define MU_SQUAD_ROOM_SQUAD 1', '#define MU_SQUAD_ROOM_WAITING 2']:
            self.assertIn(define, wire)
        # Op 11 reaches the Slippi squad handler: the source host only takes its own ops.
        online = read('port/runtime/hle/slippi_online.cpp')
        self.assertIn('case MU_SQUAD_OP_FRIEND:', online)

    def test_only_a_one_friend_direct_room_can_adopt(self):
        net = read('port/runtime/hle/slippi_net.cpp')
        self.assertIn('squad_friend_room_ = squad_friend_room && remote_player_count == 1 && player_idx < 2;', net)
        self.assertIn('!search_settings_.friend_room.empty() && search_settings_.mode == DIRECT', net)
        send = body(net, 'void NetplayClient::SendBuild(')
        self.assertIn('caps = yougame::squad::caps(caps, 1, squad_search_.load());', send)
        self.assertIn('if (squad_friend_room_) caps |= MU_SQUAD_FRIEND_CAP;', send)

    def test_build_adopts_or_refuses_before_picks(self):
        net = read('port/runtime/hle/slippi_net.cpp')
        build = net[net.index('const bool second_build = squad_peer_build_.load();'):]
        build = build[:build.index('squad_peer_build_.store(true); // heard last')]
        adopt = build.index('if (squad_friend_room_ && !squad_search_.load() && (caps & MU_SQUAD_CAP) && !squad_refused_.load()) {')
        refuse = build.index('} else if (squad_friend_room_ && squad_search_.load() && !(caps & (MU_SQUAD_CAP | MU_SQUAD_FRIEND_CAP))) {')
        self.assertLess(adopt, refuse)
        joined = build[adopt:refuse]
        # Never adopted after a lock-in went either way or a game started: the room cancels.
        late = joined.index('if (squad_locked_seen_.load()) {')
        self.assertLess(late, joined.index('squad_search_.store(true);'))
        self.assertIn('CancelSquad("squad: friend\'s Squad Strike arrived after a lock-in, match cancelled");', joined)
        self.assertIn("Your friend picked Squad Strike after a lock-in. Match not started.", joined)
        for step in ['squad_search_.store(true);', 'squad_adopted_.store(true);',
                     'host::log("squad: friend picked Squad Strike, joining it");', 'SendBuild();']:
            self.assertIn(step, joined)
        # The build mark comes last: until then the joiner's lock-in is held, never read as plain.
        self.assertIn('const bool second_build = squad_peer_build_.load();', build)
        self.assertLess(joined.index('squad_adopted_.store(true);'), joined.index('squad_search_.store(true);'))
        mark = net.index('squad_peer_build_.store(true); // heard last')
        self.assertLess(net.index('host::log("squad: friend picked Squad Strike, joining it");'), mark)
        self.assertIn(': "squad: friend\'s game has no friend Squad Strike, match cancelled");', build[refuse:])
        self.assertIn("Your friend's game can't play Squad Strike. Match not started.", build[refuse:])
        # A later build without either bit, from a game that had the friend bit, is a refusal.
        self.assertIn('const bool refused = second_build && had_friend_cap;', build[refuse:])
        self.assertIn("Your friend's game can't load Squad Strike. Match not started.", build[refuse:])
        for mark in ['squad_locked_seen_.store(true); // a friend', 'if (match_info_.local.is_character_selected) squad_locked_seen_.store(true);',
                     'if (s->is_character_selected) squad_locked_seen_.store(true);']:
            self.assertIn(mark, net)
        refuse_fn = body(net, 'void NetplayClient::SquadFriendRefuse() {')
        self.assertIn('if (!squad_friend_room_ || squad_refused_.exchange(true)) return;', refuse_fn)
        self.assertIn('SendBuild();', refuse_fn)
        self.assertIn('if (squad_refused_.load()) caps &= (uint8_t)~(MU_SQUAD_CAP | MU_SQUAD_FRIEND_CAP);', body(net, 'void NetplayClient::SendBuild('))
        self.assertIn('"Your friend locked in before their game switched to Squad Strike. Match not started."', net)

    def test_host_answers_query_and_invite(self):
        online = read('port/runtime/hle/slippi_online.cpp')
        case = online[online.index('case MU_SQUAD_OP_FRIEND:'):]
        case = case[:case.index('default: return false;')]
        self.assertIn('const bool room = !g_last_search.friend_room.empty() && g_last_search.mode == Matchmaking::DIRECT;', case)
        self.assertIn('const bool adopted = room && g_netplay && g_netplay->SquadAdopted();', case)
        self.assertIn('const bool waiting = !g_netplay || g_netplay->SquadFriendWaiting();', case)
        self.assertLess(case.index('const bool waiting ='), case.index('const bool adopted ='))
        self.assertIn(': waiting ? MU_SQUAD_ROOM_WAITING', case)
        self.assertIn('const bool live = room && g_last_search.squad && slippi_session_up();', case)
        self.assertIn('q.assign(1, g_squad_friend_intent || !g_squad_friend_room.empty() || live ? 1 : 0);', case)
        self.assertIn('g_squad_friend_intent = yougame::friends::events.friend_entry();', case)
        self.assertIn('if (told) g_netplay->SquadFriendRefuse();', case)
        self.assertIn('g_last_search.squad = true;', case)
        self.assertIn('host::log("squad: friend room squad mode (friend\'s pick), character select told");', case)
        self.assertIn('yougame::friends::events.request_host();', case)
        self.assertIn('host::log(g_squad_friend_intent ? "squad: play a friend, opening the friend picker"', case)

    def test_css_and_lock_in_follow_the_friend_room(self):
        squad = read('sourceport/game/shim/mu_squad_online.c')
        live = body(squad, 'bool gmSquad_OnlineLive(int mode)')
        self.assertIn('online_friend ? mode == MU_SLP_MODE_DIRECT : mode == MU_SLP_MODE_UNRANKED', live)
        join = body(squad, 'bool gmSquad_OnlineFriendJoin(void)')
        self.assertIn('if (online_chosen || match->local_ready) return false;', join)
        self.assertLess(join.index('gmSquad_OnlineChooseFriend(true);'), join.index('gmSquad_OnlineCss();'))
        choose = body(squad, 'void gmSquad_OnlineChoose(bool chosen)')
        self.assertLess(choose.index('online_friend = false;'), choose.index('if (chosen == online_chosen) return;'))
        # Lock-in: a full lineup always, and in a friend room the memory floors too (no search did).
        sss = read('sourceport/game/shim/mu_slippi_sss.c')
        # A held lock-in (waiting for the friend's game) still searches (-1); a refused one (0) does not.
        self.assertIn('if (s->mode == MU_SLP_MODE_DIRECT && !gmSquad_OnlineFriendLockIn())\n'
                      '        return gmSquad_OnlineFriendHeld() ? -1 : 0;', sss)
        self.assertLess(sss.index('if (s->mode == MU_SLP_MODE_DIRECT && !gmSquad_OnlineFriendLockIn())'),
                        sss.index('if (gmSquad_OnlineLive(s->mode)) {'))
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        prep = body(menu, 'static void css_prep(GameModeState* state)')
        self.assertLess(prep.index('gmSquad_OnlineRoom(slp.mode);'), prep.index('if (gmSquad_OnlineChosen()) gmSquad_OnlineCss();'))
        gate = sss[sss.index('if (gmSquad_OnlineLive(s->mode)) {'):]
        gate = gate[:gate.index('\n    }')]
        self.assertIn('if (!gmSquad_OnlineSelect(', gate)
        self.assertIn('if (gmSquad_OnlineFriend() && !gmSquad_OnlineBudget()) return 0;', gate)
        css = read('sourceport/game/shim/mu_slippi_css.c')
        self.assertIn('if (!gmSquad_OnlineLive(mu_slippi_state()->mode)) return;', css)
        self.assertIn('gmSquad_OnlineFriendJoin();', css)
        self.assertIn('if (gmSquad_OnlineFriend()) { set_text(ST_HEADER, "Squad Strike"); break; }', css)

    def test_menu_y_on_the_squad_row_opens_friends(self):
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        y = menu[menu.index('"Play a friend": Y on the Squad Strike row'):]
        y = y[:y.index('#endif')]
        self.assertIn('mu_slippi_menu_option(mn_804A04F0.hovered_selection) == MU_SLP_OPT_SQUAD', y)
        self.assertIn('if (!(input[port][1] & 0x0800)) continue;', y)
        self.assertIn('if (!mn_80229938(MENU_KIND_8, friends_row)) {', y)
        self.assertIn('OSReport("squad: play a friend refused, Friends is locked\\n");', y)
        order = ['gmSquad_OnlineChooseFriend(false);', 'mn_804A04F0.hovered_selection = (u8) friends_row;',
                 'squad_friend_invite = 1;', 'OSReport("squad: play a friend from the Squad Strike row (pad %d)\\n", port + 1);']
        at = [y.index(step) for step in order]
        self.assertEqual(at, sorted(at))
        self.assertLess(menu.index('"Play a friend": Y on the Squad Strike row'), menu.index('selected_row = mn_804A04F0.hovered_selection;'))
        after = menu[menu.index('if (gmSquad_OnlineFriend() && (!friends_visible || !friends_hovered)) {'):]
        after = after[:after.index('#endif')]
        self.assertIn('OSReport("squad: friend room squad mode off (left the Friends row)\\n");', after)
        self.assertIn('u8 invite[2] = { MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_INVITE }', after)
        self.assertIn('OSReport("squad: play a friend, the host did not open the friend picker\\n");', after)
        self.assertIn('if (gmSquad_OnlineFriend()) mu_slippi_tournament_notice(gp, "Invite a friend to Squad Strike.");', after)
        self.assertIn('u8 intent[2] = { MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_INTENT }', after)
        self.assertIn('OSReport("squad: friend picker closed, Squad Strike off\\n");', after)
        patch = (ROOT / 'sourceport/patches/melee-native.patch').read_bytes()
        self.assertIn(b'4 fighters each. A: search, Y: play a friend.', patch)


class FriendsSquadCss(unittest.TestCase):
    """The production character-select rules for a friend room, against a stand-in host."""

    def test_room_join_lock_in_and_refusal(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('needs a C compiler')
        source = read('sourceport/game/shim/mu_squad_online.c')
        start = source.index('static bool online_chosen, online_ready')
        end = source.index('bool gmSquad_OnlineFriendLockIn(void)')
        end += len(body(source[end:], 'bool gmSquad_OnlineFriendLockIn(void)')) + len('bool gmSquad_OnlineFriendLockIn(void)\n')
        program = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mu_squad_wire.h"
#include "mu_lobby_wire.h"
static bool gmSquad_OnlineBudget(void) { return true; }
static int mu_lobby_send(const unsigned char* p, unsigned n) { (void)p; (void)n; return 1; }
typedef unsigned char u8;
typedef unsigned int u32;
#define GMSQUAD_SIZE MU_SQUAD_LINEUP
#define GMSQUAD_EXTRA (GMSQUAD_SIZE - 1)
typedef struct { u8 ck, color; } GmSquadMember;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT };
enum { MU_SLP_MM_IDLE, MU_SLP_MM_INITIALIZING, MU_SLP_MM_MATCHMAKING, MU_SLP_MM_OPPONENT_CONNECTING,
       MU_SLP_MM_CONNECTION_SUCCESS, MU_SLP_MM_ERROR };
typedef struct { unsigned char connection_state, local_ready; } MuMatchState;
typedef struct { int mode; } MuSlippiMenuState;
static MuMatchState match;
static MuSlippiMenuState menu;
static const MuMatchState* mu_slippi_match_state(void) { return &match; }
static MuSlippiMenuState* mu_slippi_state(void) { return &menu; }
static char logs[4096];
static void OSReport(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(logs + strlen(logs), sizeof logs - strlen(logs), format, args);
    va_end(args);
}
static bool logged(const char* line) { return strstr(logs, line) != NULL; }
static int answer = -1, refusals, css_opens, clears;
static void gmSquad_ClearAll(void) { ++clears; }
static void gmSquad_Disable(void) {}
static void gmSquad_OnlineCss(void) { ++css_opens; }
static int mu_slippi_cmd(unsigned cmd, const void* payload, unsigned size, void* reply, unsigned cap, unsigned* got)
{
    const u8* p = payload;
    assert(cmd == MU_SQUAD_COMMAND && cap == 1);
    if (p[0] == 0) { assert(size == 2); *(u8*) reply = 1; *got = 1; return 0; }
    assert(p[0] == MU_SQUAD_OP_FRIEND && size == 2);
    if (p[1] == MU_SQUAD_FRIEND_QUERY) {
        if (answer < 0) { *got = 0; return 1; }
        *(u8*) reply = (u8) answer; *got = 1; return 0;
    }
    assert(p[1] == MU_SQUAD_FRIEND_REFUSE);
    ++refusals; *(u8*) reply = 1; *got = 1; return 0;
}
""" + source[start:end] + r"""
static void room(int mode, int state) { menu.mode = mode; match.connection_state = (u8) state; match.local_ready = 0; }
int main(void)
{
    /* 1. Squad Strike picked on the Unranked Squad CSS, then a friend's invite joins a friend room
     * whose friend plays a 1v1: the leftover choice is dropped at the room's CSS, never kept. */
    room(MU_SLP_MODE_UNRANKED, MU_SLP_MM_IDLE);
    gmSquad_OnlineChoose(true);
    assert(gmSquad_OnlineLive(MU_SLP_MODE_UNRANKED) && !gmSquad_OnlineFriend());
    room(MU_SLP_MODE_DIRECT, MU_SLP_MM_OPPONENT_CONNECTING);
    gmSquad_OnlineRoom(MU_SLP_MODE_DIRECT);
    assert(!gmSquad_OnlineChosen() && logged("squad: Squad Strike off, not this character select's mode"));
    answer = MU_SQUAD_ROOM_WAITING;
    assert(!gmSquad_OnlineFriendJoin()); /* not connected yet */
    match.connection_state = MU_SLP_MM_CONNECTION_SUCCESS;
    assert(!gmSquad_OnlineFriendJoin() && !gmSquad_OnlineChosen());
    /* Until the friend's game is heard from, lock-in waits and says so (with nothing chosen). */
    assert(!gmSquad_OnlineFriendLockIn());
    assert(!strcmp(gmSquad_OnlineError(), "Waiting for your friend's game..."));
    assert(logged("squad: lock-in held, waiting for your friend's game"));
    answer = MU_SQUAD_ROOM_PLAIN;
    assert(!gmSquad_OnlineFriendJoin() && !gmSquad_OnlineError()[0]);
    assert(gmSquad_OnlineFriendLockIn()); /* both plain: a 1v1 both of them picked */

    /* 2. The same join when the friend picked Squad Strike: this side joins it, 4 fighters each. */
    gmSquad_OnlineChoose(false); room(MU_SLP_MODE_UNRANKED, MU_SLP_MM_IDLE);
    gmSquad_OnlineChoose(true);
    room(MU_SLP_MODE_DIRECT, MU_SLP_MM_CONNECTION_SUCCESS);
    gmSquad_OnlineRoom(MU_SLP_MODE_DIRECT);
    assert(!gmSquad_OnlineChosen());
    answer = MU_SQUAD_ROOM_SQUAD; css_opens = 0;
    assert(gmSquad_OnlineFriendJoin() && gmSquad_OnlineFriend() && gmSquad_OnlineLive(MU_SLP_MODE_DIRECT));
    assert(css_opens == 1 && gmSquad_OnlineFriendLockIn());

    /* 3. Leftover "Play a friend" state from an earlier friend room, then another friend's room
     * (plain until that friend says otherwise): the CSS drops it before anything is shown. */
    gmSquad_OnlineChoose(false);
    gmSquad_OnlineChooseFriend(false);
    assert(gmSquad_OnlineFriend());
    answer = MU_SQUAD_ROOM_WAITING;
    room(MU_SLP_MODE_DIRECT, MU_SLP_MM_OPPONENT_CONNECTING);
    gmSquad_OnlineRoom(MU_SLP_MODE_DIRECT);
    assert(!gmSquad_OnlineChosen() && logged("squad: Squad Strike off, this friend room is not Squad Strike"));
    /* And this side's own "Play a friend" room keeps it (the host answers squad at once). */
    gmSquad_OnlineChooseFriend(false); answer = MU_SQUAD_ROOM_SQUAD;
    gmSquad_OnlineRoom(MU_SLP_MODE_DIRECT);
    assert(gmSquad_OnlineFriend());
    /* An Unranked Squad CSS keeps its own choice. */
    gmSquad_OnlineChoose(false); gmSquad_OnlineChoose(true);
    gmSquad_OnlineRoom(MU_SLP_MODE_UNRANKED);
    assert(gmSquad_OnlineChosen() && !gmSquad_OnlineFriend());

    /* 4. Lock-in races: the friend's pick arrives at lock-in (refused, squads on, picks needed);
     * a squad choice in a room that turned out plain is refused and dropped, never a silent 1v1. */
    gmSquad_OnlineChoose(false);
    room(MU_SLP_MODE_DIRECT, MU_SLP_MM_CONNECTION_SUCCESS);
    answer = MU_SQUAD_ROOM_SQUAD; css_opens = 0;
    assert(!gmSquad_OnlineFriendLockIn() && gmSquad_OnlineFriend() && css_opens == 1);
    assert(logged("squad: lock-in refused, your friend picked Squad Strike"));
    answer = MU_SQUAD_ROOM_PLAIN;
    assert(!gmSquad_OnlineFriendLockIn() && !gmSquad_OnlineChosen());
    assert(logged("squad: lock-in refused, this friend room is not Squad Strike"));
    /* An old host with no friend answer: plain rooms lock in as before. */
    answer = -1;
    assert(gmSquad_OnlineFriendLockIn());

    /* 5. This game can't load Squad Strike in the friend's room: the friend is told once. */
    answer = MU_SQUAD_ROOM_SQUAD; refusals = 0;
    gmSquad_OnlineChooseFriend(true);
    search_refused = true;
    match.local_ready = 0;
    assert(!gmSquad_OnlineFriendJoin() && refusals == 1);
    assert(!gmSquad_OnlineFriendJoin() && refusals == 1);
    assert(logged("squad: this game can't load Squad Strike, friend told"));
    gmSquad_OnlineChoose(false);
    assert(refusals == 1);
    puts("friends squad css: PASS");
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix='friends-squad-css-') as directory:
            work = Path(directory)
            (work / 'css.c').write_text(program)
            exe = work / 'css'
            built = subprocess.run([cc, '-std=c11', '-DMU_SQUAD=1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                    '-Wno-unused-variable', '-Wno-unused-parameter',
                                    '-I', str(ROOT / 'port/runtime/abi'), str(work / 'css.c'), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('PASS', out.stdout)


class FriendsLobbySquad(unittest.TestCase):
    def test_squad_lobby_keeps_four_doors_and_no_doubles_panel(self):
        # Squad Strike runs in the 4-door Friends lobby: Build 91's two-door hide and the partner panel key on
        # MU_LOBBY_M_DOUBLES alone, so the Squad select draws as before.
        inc = read('sourceport/game/include/mu_friends4_css.inc')
        frame = body(inc, 'static void mu_friends4_css_frame(void)')
        # Same condition gates the two-door hide (build 95: nothing moves the doors).
        self.assertIn('if (mu_f4.view_ok ? mu_f4_doubles() : mu_f4.shut) mu_f4_shut();', frame)
        self.assertNotIn('MU_LOBBY_M_SQUAD', body(inc, 'static void mu_f4_shut(void)'))
        panel = body(inc, 'static void mu_f4_panel(const MuLobbyView* v, int doubles')
        self.assertIn('if (!doubles || mu_lobby_curtain()) {', panel)
        # The Squad hint keeps line 0 and the door labels; build 96's panel writes in Doubles only.
        self.assertIn('#define MU_F4_LINES 6', inc)
        texts = body(inc, 'static void mu_f4_texts(')
        self.assertIn('mu_f4_line(2 + i, buf);', texts)
        self.assertIn('const int doubles = v ? (v->mode & MU_LOBBY_M_DOUBLES) != 0 : mu_f4.shut;', texts)
        self.assertLess(texts.index('if (doubles) {'), texts.index('squad_note[mine]'))
        self.assertIn('if (!doubles) mu_f4_panel(v, 0, hint, noted, "", mine);', texts)
        self.assertNotIn('mu_f4_line(', panel)

    def test_toggle_ownership_and_refused_start_before_vanilla_ready(self):
        inc = read('sourceport/game/include/mu_friends4_css.inc')
        program = r"""
#include <cassert>
#include <cstdio>
#include "mu_lobby_wire.h"
using u8 = unsigned char; using u32 = unsigned;
constexpr unsigned HSD_PAD_START = 1;
struct { MuLobbyView view{}; int view_ok = 1, leaving = 0, mode_pending = 0; unsigned mode_seen = 0; } mu_f4;
int notes, requests, exits, errors, last_note;
int doubles;
int mu_lobby_css() { return 1; }
int mu_f4_doubles() { return doubles; }
int mu_f4_searching() { return 0; }
int mu_f4_host() { return (mu_f4.view.flags & MU_LOBBY_F_HOST) && !(mu_f4.view.flags & MU_LOBBY_F_STARTED); }
void mu_f4_note(unsigned n) { ++notes; last_note = n; }
int mu_f4_partner_start() { return 0; }   // Doubles' partner tab (doubles_partner_cycle_test.py)
void lbAudioAx_80024030(int) { ++errors; }
void sfxMove() {} void sfxForward() {}
constexpr int MU_F4_HOLD = 60;
int mu_lobby_send(const unsigned char* p, unsigned) { ++requests; assert(*p == MU_LOBBY_OP_CYCLE_MODE || *p == MU_LOBBY_OP_STAGE_SELECT); return 1; }
void OSReport(const char*, ...) {}
constexpr int MU_LOBBY_EXIT_SSS = 1;
void mu_lobby_css_exit(int) { ++exits; }
int mnCharSel_804D6CF6, mnCharSel_804D6CF2;
void gm_801A4B60() {}
"""
        for name, decl in [('mu_friends4_host_only', 'static int mu_friends4_host_only(void)'),
                           ('mu_friends4_cycle_mode', 'static int mu_friends4_cycle_mode(void)'),
                           ('mu_friends4_css_start', 'static u32 mu_friends4_css_start(u32 trigger, int ready)')]:
            program += decl + body(inc, decl) + '\n'
        program += r"""
int main() {
  mu_f4.view.flags = MU_LOBBY_F_HOST;
  assert(mu_friends4_cycle_mode() && requests == 1);
  mu_f4.mode_pending = 0; mu_f4.view.flags = 0;
  assert(mu_friends4_cycle_mode() && requests == 1 && last_note == MU_LOBBY_HINT_HOST_ONLY);
  doubles = 1; mu_f4.view.flags = MU_LOBBY_F_HOST;
  assert(mu_friends4_cycle_mode() && requests == 1 && last_note == MU_LOBBY_HINT_FIXED_RULES);
  doubles = 0;
  for (int roster : {3,4,5}) {
    (void)roster;
    mu_f4.view.mode = MU_LOBBY_M_SQUAD; mu_f4.mode_seen = MU_LOBBY_M_SQUAD;
    mu_f4.view.block_hint = MU_LOBBY_HINT_SQUAD_TWO;
    notes = 0;
    assert(!mu_friends4_css_start(HSD_PAD_START, 0));
    assert(notes == 1 && last_note == MU_LOBBY_HINT_SQUAD_TWO && requests == 1 && !exits);
  }
  mu_f4.view.block_hint = MU_LOBBY_HINT_PICK; notes = 0;
  mu_friends4_css_start(HSD_PAD_START, 0);
  assert(notes == 1 && last_note == MU_LOBBY_HINT_PICK && requests == 1);
  mu_f4.view.block_hint = MU_LOBBY_HINT_PRESS_START;
  for (int pending : {0,1}) {
    mu_f4.mode_pending = pending; mu_f4.mode_seen = pending ? MU_LOBBY_M_SQUAD : 0;
    mu_friends4_css_start(HSD_PAD_START, 1);
    assert(last_note == MU_LOBBY_HINT_CHANGED && requests == 1 && !exits);
  }
  mu_f4.mode_pending = 0; mu_f4.mode_seen = 0;
  // Ordinary Melee still waits for vanilla's readiness delay.
  mu_f4.view.mode = 0; mu_f4.view.block_hint = MU_LOBBY_HINT_PRESS_START;
  mu_friends4_css_start(HSD_PAD_START, 0); assert(requests == 1 && !exits);
  mu_friends4_css_start(HSD_PAD_START, 1); assert(requests == 2 && exits == 1);
}
"""
        with tempfile.TemporaryDirectory(prefix='friends-lobby-hooks-') as directory:
            work = Path(directory)
            (work/'hooks.cpp').write_text(program.replace("}\\n", "}\n"))
            result = subprocess.run(['c++', '-std=c++17', '-DMU_SQUAD=1', '-include', 'initializer_list',
                                     '-I', str(ROOT/'port/runtime/abi'), str(work/'hooks.cpp'), '-o', str(work/'hooks')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(work/'hooks')], check=True)

    def test_splash_uses_frozen_mode_with_stale_css_choice(self):
        menu = read('sourceport/game/shim/mu_slippi_menu.c')
        program = r"""
#include <cassert>
#include "mu_squad_wire.h"
struct Start { int unused; }; struct { Start start; } vs;
struct { int game_info; } last_match;
int chosen, frozen, imports;
int mu_slippi_load_match_state(void*) { return 0; }
void mu_replay_apply_game_info(Start*, int) {}
auto gmVsMelee_GetVsData() { return &vs; }
int mu_lobby_css() { return 1; }
int mu_slippi_cmd(int cmd,const unsigned char* q,unsigned n,unsigned char* out,unsigned,void*) {
 assert(cmd==MU_SQUAD_COMMAND && *q==MU_SQUAD_OP_LOBBY_MODE && n==1); *out=frozen; return 0;
}
void gmSquad_LobbyChoose(int c) { chosen=c; }
int gmSquad_LobbyChosen() { return chosen; }
int gmSquad_OnlineChosen() { return 0; }
void gmSquad_OnlineSetup(Start*) { ++imports; }
"""
        program += 'void mu_slippi_splash_init(void)' + body(menu, 'void mu_slippi_splash_init(')
        program += r"""
int main() {
 for(int mode: {0,1}) for(int css: {0,1}) {
  frozen=mode; chosen=css; imports=0; mu_slippi_splash_init();
  assert(chosen==mode && imports==mode);
 }
}
"""
        with tempfile.TemporaryDirectory(prefix='friends-frozen-') as directory:
            work=Path(directory); (work/'test.cpp').write_text(program)
            built=subprocess.run(['c++','-std=c++17','-DMU_SQUAD=1','-include','initializer_list',
                                  '-I',str(ROOT/'port/runtime/abi'),str(work/'test.cpp'),'-o',str(work/'test')],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            subprocess.run([str(work/'test')],check=True)

    def test_room_format_and_seat_sync(self):
        inc = read('sourceport/game/include/mu_friends4_css.inc')
        program = r"""
#include <cassert>
#include <cstring>
#include "mu_lobby_wire.h"
struct CSSData { int pick = -1; };
struct { MuLobbyView view{}; int view_ok=1, rules_sync=0, rules_drawn_ok=0, mode_pending=0;
 unsigned char mode_seen=255, seat_seen=255, sent[4]{}, lineup_sent[10]{};
 int refusal_ok=0, refusal_seq=0; } mu_f4;
constexpr int GMSQUAD_EXTRA=3, GMSQUAD_NONE=255;
struct Member { int ckind=255, color=0; } gmSquad_rows[4][3];
int gmSquad_coin_pos[4], squad_coin_holder[4], held[4], chosen=0, clears=0;
void gmSquad_LobbyChoose(int c) { chosen=c; }
int gmSquad_LobbyChosen() { return chosen; }
void gmSquad_ClearAll() { ++clears; for(auto& row:gmSquad_rows) for(auto& m:row) m.ckind=255; }
int mu_squad_color(int, int c) { return c; }
void mu_f4_draw(CSSData* css,int p,const MuLobbySeat* s) { css->pick=(s->flags & MU_LOBBY_S_PICKED)?s->pick_ch:-1; held[p]=css->pick<0; }
void squad_regrab(int p,int) { held[p]=1; }
int squad_coin_held(int p) { return held[p]; }
#include <cstdarg>
#include <cstdio>
#include <string>
std::string logged;
void OSReport(const char* f, ...) { char b[160]; va_list a; va_start(a,f); vsnprintf(b,sizeof b,f,a); va_end(a); logged+=b; }
"""
        program += 'static void mu_f4_sync_mode(CSSData* css, int mine)' + body(inc, 'static void mu_f4_sync_mode(')
        program += r"""
int main() {
 CSSData css; mu_f4.view.my_seat=255; mu_f4.view.mode=MU_LOBBY_M_SQUAD;
 mu_f4_sync_mode(&css,0); assert(mu_f4.mode_seen==255 && clears==0);
 mu_f4.view.my_seat=1; auto& seat=mu_f4.view.seats[1]; seat.kind=MU_LOBBY_KIND_HUMAN;
 seat.lineup_count=4; for(int j=0;j<4;++j) seat.lineup[j][0]=j;
 mu_f4_sync_mode(&css,1); assert(css.pick==3 && !held[1] && gmSquad_rows[1][0].ckind==0);
 // Same format, new seat: restore only after this player actually owns it.
 mu_f4.view.my_seat=2; mu_f4.view.seats[2]=seat; mu_f4.view.seats[2].lineup[0][0]=8;
 mu_f4_sync_mode(&css,2); assert(gmSquad_rows[2][0].ckind==8 && mu_f4.seat_seen==2);
 // The server flips Squad off and clears both picks and lineups.
 mu_f4.view.mode=0; mu_f4.view.seats[2].flags=0; mu_f4.view.seats[2].lineup_count=0;
 mu_f4_sync_mode(&css,2); assert(css.pick==-1 && held[2] && !chosen);
 for(auto& row:gmSquad_rows) for(auto& m:row) assert(m.ckind==255);
 assert(mu_f4.sent[0]==255 && mu_f4.lineup_sent[0]==255);
 // And on again: no stale ordinary coin becomes the fourth fighter.
 css.pick=7; held[2]=0; mu_f4.view.mode=MU_LOBBY_M_SQUAD;
 mu_f4_sync_mode(&css,2); assert(css.pick==-1 && held[2] && chosen);
 // Each format change is logged once (a seat change alone is not).
 assert(logged=="friends lobby: mode Squad Strike\nfriends lobby: mode Free-for-all\nfriends lobby: mode Squad Strike\n");
 // A new visit (back from Rules or the stage select) resets mode_seen: the same mode is not logged again.
 mu_f4.mode_seen=255; mu_f4_sync_mode(&css,2);
 // Teams toggled inside Squad Strike changes mode_seen's teams bit, not the mode the log names.
 mu_f4.view.rules.teams=1; mu_f4_sync_mode(&css,2);
 assert(logged=="friends lobby: mode Squad Strike\nfriends lobby: mode Free-for-all\nfriends lobby: mode Squad Strike\n");
 // Teams in Free-for-all is Team Battle: logged.
 mu_f4.view.mode=0; mu_f4_sync_mode(&css,2);
 assert(logged=="friends lobby: mode Squad Strike\nfriends lobby: mode Free-for-all\nfriends lobby: mode Squad Strike\nfriends lobby: mode Team Battle\n");
}
"""
        frame = body(inc, 'static void mu_friends4_css_frame(')
        self.assertLess(frame.index('mu_f4_sync_mode(css, mine);'), frame.index('mu_f4_local(css, mine);'))
        with tempfile.TemporaryDirectory(prefix='friends-format-') as directory:
            work = Path(directory); (work/'test.cpp').write_text(program)
            built = subprocess.run(['c++', '-std=c++17', '-DMU_SQUAD=1', '-I', str(ROOT/'port/runtime/abi'),
                                    str(work/'test.cpp'), '-o', str(work/'test')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(work/'test')], check=True)

    def test_local_picker_remote_images_and_no_plain_launch(self):
        inc = read('sourceport/game/include/mu_friends4_css.inc')
        css = read('sourceport/game/shim/mu_squad_css.c')
        online = read('port/runtime/hle/slippi_online.cpp')
        local = body(inc, 'static void mu_f4_local(')
        branch = body(local, 'if (gmSquad_LobbyChosen())')
        self.assertIn('MU_LOBBY_OP_LINEUP', branch)
        self.assertIn('gmSquad_Lineup(mine,', branch)
        self.assertNotIn('MU_LOBBY_OP_PICK', branch)
        self.assertIn('return;', branch)
        frame = body(css, 'static void mnCharSel_SquadFrame(')
        lobby = body(frame, 'if (mu_lobby_css())')
        self.assertIn('p = v->my_seat;', lobby)
        self.assertIn('squad_fill_on_drop(p);', lobby)
        self.assertIn('squad_buttons(p);', lobby)
        self.assertNotIn('for (p', lobby)
        remote = body(css, 'static bool squad_icon_row(')
        self.assertIn('p != v->my_seat', remote)
        self.assertNotIn('mu_lobby_read()', remote)
        self.assertIn('squad_icon_row(p, v)', css)
        self.assertIn('seat->lineup[i][0]', remote)
        self.assertIn('seat->lineup[i][1]', remote)
        self.assertIn('squad_grid_marks(v->my_seat)', css)
        texts = body(inc, 'static void mu_f4_texts(')
        self.assertIn('squad_note[mine] && mu_f4.note_frames == 0', texts)
        banners = body(body(css, 'static void squad_banners('), 'if (mu_lobby_css())')
        self.assertNotIn('squad_line(', banners)  # no new white lineup text
        self.assertIn('return gmSquad_LobbyChosen() ||', body(css, 'static bool squad_fill_mode(void)\n'))
        self.assertIn('mu_baked_squad', body(read('sourceport/game/shim/mu_menu_label.c'), 'void mu_squad_lobby_label_draw('))
        menu = body(read('sourceport/game/shim/mu_slippi_menu.c'), 'static int lobby_css_decide(')
        self.assertIn('gmSquad_LobbyChosen() && !gmSquad_OnlineReady()', menu)
        failed = body(menu, 'if (gmSquad_LobbyChosen() && !gmSquad_OnlineReady())')
        self.assertNotIn('lobby.played = 0', failed)
        self.assertIn('last_match.connection_state != MU_SLP_MM_CONNECTION_SUCCESS', menu)
        init = body(read('sourceport/game/shim/mu_slippi_menu.c'), 'void mu_slippi_splash_init(')
        self.assertIn('MU_SQUAD_OP_LOBBY_MODE', init)
        self.assertLess(init.index('gmSquad_LobbyChoose(squad != 0)'), init.index('gmSquad_OnlineSetup('))
        self.assertIn('gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);', menu)
        self.assertIn('lobby_plan->squad_budget', online)
        self.assertIn('yougame::lobby::squad_setup(*lobby_plan, &squad)', online)
        # The lobby retains its ordinary key path; only the frozen setup gets squad facts.
        key = body(read('port/runtime/hle/yougame_link.cpp'), 'std::string lobby_room_key(')
        self.assertIn('return compatibility_key(', key)
        self.assertNotIn('build_tag', key)
        self.assertNotIn('squad', key)

if __name__ == '__main__':
    unittest.main()
