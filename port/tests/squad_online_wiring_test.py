"""Squad Strike's optional online path: offline production fixtures, never service I/O.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
"""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from squad_wiring_test import CREDIT, GAME, DECOMP, NEEDS_DECOMP, ROOT, function, read, without_squad, production_fixture


def source(path):
    return read(ROOT / path)


def run(args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise AssertionError(' '.join(map(str, args)) + '\n' + result.stdout + result.stderr)
    return result.stdout


class SquadOnlineWiring(unittest.TestCase):
    def test_menu_row_and_game_hooks_are_optional(self):
        menu = read(GAME / 'include/mu_online_menu.h')
        rows = menu.split('mu_online_top_rows[] = {', 1)[1].split('};', 1)[0]
        self.assertLess(rows.index('MU_SLP_MODE_UNRANKED'), rows.index('MU_SLP_OPT_SQUAD'))
        self.assertLess(rows.index('MU_SLP_OPT_SLIPPI'), rows.index('MU_SLP_OPT_SQUAD'))
        self.assertNotIn('SQUAD', without_squad(menu))
        for name in ['shim/mu_slippi_menu.c', 'shim/mu_slippi_css.c', 'shim/mu_slippi_sss.c',
                     'shim/mu_menu_label.c', 'include/mu_native.h']:
            text = read(GAME / name)
            self.assertIn(CREDIT, text)
            self.assertNotRegex(without_squad(text), r'gmSquad|MU_SLP_OPT_SQUAD|mu_baked_squad|mu_squad')
        self.assertIn('Squad Strike by Chris (x9x9x9x9x9) - idea: Tyler', read(GAME / 'shim/mu_slippi_css.c'))
        self.assertIn('gmSquad_OnlineChosen()) slp.mode = MU_SLP_MODE_UNRANKED', read(GAME / 'shim/mu_slippi_menu.c'))

    @NEEDS_DECOMP
    def test_decomp_row_uses_native_unranked_animation(self):
        text = read(DECOMP / 'mn/mnmain.c')
        self.assertIn('if (option == MU_SLP_OPT_SQUAD) source = MU_SLP_MODE_UNRANKED;', text)
        self.assertNotIn('MU_SLP_OPT_SQUAD', without_squad(text))

    def test_caps_are_only_the_existing_byte_and_lineup_precedes_selections(self):
        net = source('port/runtime/hle/slippi_net.cpp')
        build = function(net, 'NetplayClient::SendBuild')
        self.assertIn('caps = yougame::squad::caps(caps, 1, squad_search_.load());', build)
        self.assertIn('if (squad_friend_room_) caps |= MU_SQUAD_FRIEND_CAP;', build)
        self.assertNotIn('squad', without_squad(build))
        self.assertLess(build.index('*p << caps;'), build.index('for (char ch : rules_digest)'))
        self.assertLess(build.index('for (char ch : rules_digest)'), build.index('const uint32_t shield'))
        selections = function(net, 'NetplayClient::SetMatchSelections')
        for word in ['NP_MSG_MU_SQUAD', 'mu_squad_encode', 'squad_transmitted_']:
            self.assertIn(word, selections)
        self.assertLess(selections.index('SendAsync(std::move(lineup))'), selections.index('WriteSelections('))
        sender = function(net, 'NetplayClient::Send')
        self.assertIn('ENET_PACKET_FLAG_RELIABLE', sender)
        self.assertIn('uint8_t channel = 0;', sender)
        self.assertNotIn('NP_MSG_MU_SQUAD', sender)  # same reliable channel as 0x82
        self.assertIn('NP_MSG_MU_SQUAD = 0xE3', source('port/runtime/hle/slippi_net.h'))
        self.assertNotIn('NP_MSG_MU_SQUAD', without_squad(source('port/runtime/hle/slippi_net.h')))

    def test_decision_is_at_ordered_selections_with_no_timeout_fallback(self):
        net = source('port/runtime/hle/slippi_net.cpp')
        receipt = net.split('case NP_MSG_SLIPPI_MATCH_SELECTIONS:', 1)[1].split('case NP_MSG_SLIPPI_CHAT_MESSAGE:', 1)[0]
        self.assertIn('mu_squad_decide(', receipt)
        # Our own lineup counts only when we searched for squads; the peer's is the pending 0xE3.
        self.assertIn('squad_search_ ? 1 : 0, squad_peer_pending_)', receipt)
        # Never a silent 1v1: a missing cap is a cancel, and a normal match is logged as neither side.
        self.assertNotIn('squad: peer has no squad cap, normal 1v1', receipt)
        self.assertIn('squad: neither side asked for squads, normal 1v1', receipt)
        for line in ['squad: peer has no squad cap, match cancelled',
                     'squad: peer sent a lineup to a normal match, match cancelled',
                     'squad: missing lineup before selections, match cancelled']:
            self.assertIn(line, receipt)
        cancel = receipt[receipt.index('if (decision == MU_SQUAD_CANCEL)'):receipt.index('if (decision == MU_SQUAD_NORMAL')]
        for text in ["Your opponent's game can't play Squad Strike. Match not started.",
                     "Your opponent picked Squad Strike and you didn't. Match not started.",
                     "Your opponent's game has a different Squad Strike version. Update both games.",
                     "Your opponent hasn't picked 4 fighters. Match not started."]:
            self.assertIn(text, cancel)
        # A lineup naming another MU_SQUAD_VERSION is told apart from a missing pick, ahead of it.
        self.assertLess(cancel.index("different Squad Strike version"), cancel.index("hasn't picked 4 fighters"))
        # The on-screen text is set before CancelSquad, which the connect failure path reads.
        self.assertLess(cancel.index('squad_cancel_text_.store('), cancel.index('CancelSquad('))
        self.assertIn('break;', cancel.split('CancelSquad(', 1)[1])
        self.assertIn('CancelSquad(', receipt)
        # A lineup sent to a normal 1v1 is held so the next 0x82 refuses the match.
        lineup = net.split('case NP_MSG_MU_SQUAD: {', 1)[1].split('case NP_MSG_MU_BUILD:', 1)[0]
        self.assertIn('packet.data()[1] != MU_SQUAD_VERSION;', lineup)
        self.assertIn('squad_peer_other_version_ = other_version;', lineup)
        self.assertIn('squad_peer_other_version_ = false;', receipt)
        stray = lineup.index('if (!squad_search_ && remote_player_count_ == 1) {')
        self.assertIn('squad_peer_pending_ = true;', lineup[stray:lineup.index('break;', stray)])
        self.assertLess(stray, lineup.index('if (!squad_search_ || remote_player_count_ != 1 || player_idx_ > 1) break;'))
        self.assertIn('ForceDisconnect(DisconnectReason::SQUAD_PROTOCOL)', function(net, 'NetplayClient::CancelSquad'))
        self.assertNotRegex(receipt, r'time_ms|ConnectedAtMs|timeout|elapsed|sleep')
        for name in ['port/runtime/abi/mu_squad_wire.h', 'sourceport/game/shim/mu_squad_online.c']:
            self.assertNotRegex(source(name), r'\b(?:time_ms|OSGetTick|OSGetTime|sleep|timeout)\s*\(')
        setup = function(net, 'NetplayClient::SquadSetup')
        self.assertIn('&squad_transmitted_', setup)
        self.assertIn('&squad_peer_', setup)
        self.assertNotIn('&squad_local_', setup)

    def test_squad_leaves_account_and_blend_paths_before_they_can_run(self):
        net = source('port/runtime/hle/slippi_net.cpp')
        search = function(net, 'Matchmaking::startMatchmaking')
        squad = search.split('#if MU_SQUAD', 1)[1].split('#endif', 1)[0]
        self.assertIn('yougame::squad::blend_allowed', squad)
        # The build-wide Slippi lock is gone: only a chosen Squad Strike search leaves the Slippi path.
        for gone in ('kSlippiLocked', 'Slippi blend off for the squad build', 'Slippi routes locked'):
            self.assertNotIn(gone, net)
            self.assertNotIn(gone, source('port/runtime/hle/yougame_squad.h'))
        self.assertIn('startYouGameMatchmaking();', squad)
        self.assertIn('return;', squad)
        self.assertLess(search.index('startYouGameMatchmaking();'), search.index('CheckSessionCompatibility'))
        self.assertNotRegex(squad, r'GrantedSlippiUser|Check(?:Online|Session)Compatibility|blendMatchmaking\(')
        # Tickets and teardown remain the one existing unranked code path.
        link = source('port/runtime/hle/yougame_link.cpp')
        self.assertEqual(link.count('{"type", "mpTicket"}, {"players", size}'), 1)
        self.assertIn('if (search.mode == 1) request["afterMatch"] = "stay";', link)
        for name in ['close_room', 'end_session', 'game_ended']:
            self.assertNotIn('squad', function(link, name))

    def test_setup_imports_both_sides_before_preload_and_restores_after_online_info(self):
        online = read(GAME / 'shim/mu_squad_online.c')
        setup = function(online, 'gmSquad_OnlineSetup')
        self.assertIn('mu_squad_decode(bytes, got, &setup)', setup)
        self.assertIn('setup.sides[p].members[i]', setup)
        self.assertIn('gmSquad_TrimOnline(start)', setup)
        self.assertNotIn('gmSquad_Lineup(', setup)
        main = read(GAME / 'shim/mu_slippi_menu.c')
        self.assertIn('gmSquad_OnlineSetup(&gmVsMelee_GetVsData()->start)', function(main, 'mu_slippi_splash_init'))
        self.assertIn('gmSquad_SoundMask()', function(main, 'splash_prep'))
        begin = function(read(GAME / 'shim/mu_online.c'), 'mu_online_start_melee')
        self.assertLess(begin.index('mu_replay_apply_game_info('), begin.index('gmSquad_ApplyLiveStart(data)'))
        budget = function(online, 'gmSquad_OnlineBudget')
        for word in ['gmSquad_GetBudgets', 'mu_squad_fighter_files', 'mu_squad_copy_files',
                     'mu_squad_effect_file', 'mu_ground_stage_file']:
            self.assertIn(word, budget)
        fact = function(online, 'budget_fact')
        for word in ['gmSquad_FileSize', 'put32(payload + 2', 'put32(payload + 6']:
            self.assertIn(word, fact)
        host = source('port/runtime/hle/slippi_online.cpp')
        self.assertIn('source_port::mods::Sha256', host)
        self.assertIn('squad::compatibility(', source('port/runtime/hle/yougame_link.cpp'))

    def test_cpp_and_c_production_codec(self):
        cxx = shutil.which('c++')
        cc = shutil.which('cc')
        if not cxx or not cc:
            self.skipTest('needs C and C++ compilers')
        with tempfile.TemporaryDirectory(prefix='squad-online-') as directory:
            work = Path(directory)
            for enabled in [False, True]:
                exe = work / ('on' if enabled else 'off')
                flags = ['-DMU_SQUAD=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if enabled else []
                run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
                     '-I', str(ROOT / 'port/runtime/hle'), '-I', str(ROOT / 'port/third_party'),
                     str(ROOT / 'port/tests/squad_online_test.cpp'), '-o', str(exe)])
                self.assertIn('PASS', run([str(exe)]))
            c = work / 'wire.c'
            c.write_text('''#include "mu_squad_wire.h"
#include <assert.h>
int main(void) {
 MuSquadSide a={4,{{2,0},{3,1},{4,2},{9,3}}},b={4,{{14,3},{18,4},{0,5},{25,4}}};
 MuSquadSide three={3,{{2,0},{3,1},{4,2}}};
 MuSquadSetup x,y; unsigned char bytes[MU_SQUAD_WIRE_MAX]; size_t n;
 _Static_assert(MU_SQUAD_VERSION == 3 && MU_SQUAD_LINEUP == 4 && MU_SQUAD_WIRE_MAX == 20, "v3");
 assert(mu_squad_combine(0,&a,1,&b,&x)); n=mu_squad_encode(&x,bytes,sizeof bytes);
 assert(n==20 && bytes[0]==3 && x.stocks==4 && mu_squad_decode(bytes,n,&y)); assert(!memcmp(&x,&y,sizeof x));
 assert(!mu_squad_combine(0,&three,1,&b,&y) && !mu_squad_combine(0,&a,1,&three,&y)); /* 3 fighters: no */
 bytes[0]=2; assert(!mu_squad_decode(bytes,n,&y)); /* a version 2 (3-fighter) payload */
 bytes[0]=1; assert(!mu_squad_decode(bytes,n,&y)); /* a version 1 payload */
 return 0;
}''')
            run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                 '-I', str(ROOT / 'port/runtime/abi'), str(c), '-o', str(work / 'c-wire')])
            run([str(work / 'c-wire')])

    def test_production_packet_methods_in_two_copies(self):
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('needs C++ compiler')
        net = source('port/runtime/hle/slippi_net.cpp')
        def method(name):
            at = net.index('NetplayClient::' + name + '(')
            begin = net.rfind('\n', 0, at) + 1
            brace = net.index('{', at)
            depth, end = 1, brace + 1
            while depth:
                depth += (net[end] == '{') - (net[end] == '}')
                end += 1
            return net[begin:end].replace('NetplayClient::', 'Capture::') + '\n'
        packet = net[net.index('void Packet::append'):net.index('// ---------------------------------------------------------------- user')]
        methods = 'namespace slippi {\n' + packet + '}\n'
        for name in ['SenderPort', 'WriteSelections', 'ReadSelections', 'SendBuild', 'SetMatchSelections',
                     'SetSquadSelection', 'SquadFriendRefuse', 'CancelSquad', 'RefuseSquad', 'SquadPeerDisconnect', 'SquadSetup']:
            methods += method(name)
        selections = net[net.index('case NP_MSG_SLIPPI_MATCH_SELECTIONS:'):net.index('case NP_MSG_SLIPPI_CHAT_MESSAGE:')]
        lineup = net[net.index('case NP_MSG_MU_SQUAD:'):net.index('#endif\n    case NP_MSG_MU_BUILD:')]
        build = net[net.index('case NP_MSG_MU_BUILD:'):net.index('case NP_MSG_MU_EARLY_PAD:')]
        for name in ['squad_costume_refusal', 'squad_standards', 'squad_console_forget', 'squad_command']:
            methods += function(source('port/runtime/hle/slippi_online.cpp'), name)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        # The production first lock-in after connection: the lineup refresh, then 0xE3/0x82.
        online_cpp = source('port/runtime/hle/slippi_online.cpp')
        refresh = 'if (g_last_search.squad) g_netplay->SetSquadSelection(g_squad_side);'
        lock_in = 'g_netplay->SetMatchSelections(g_local_selections);'
        at = online_cpp.index(refresh)
        self.assertEqual(online_cpp.count(refresh), 1)
        body = online_cpp[at:online_cpp.index(lock_in, at) + len(lock_in)]
        # Only the refresh and the lock-in lie between them (the closing #endif of MU_SQUAD).
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        self.assertEqual(lines, [refresh, '#endif', lock_in])
        methods += 'void first_lock_in() {\n' + refresh + '\n' + lock_in + '\n}\n'
        methods += ('void Capture::Deliver(const std::vector<uint8_t>& bytes) {\n'
                    'Packet packet(bytes.data(),bytes.size()); uint8_t mid=0; packet >> mid;\n'
                    'switch(mid) {\n' + selections + lineup + build + 'default: break; }\n}\n')
        with tempfile.TemporaryDirectory(prefix='squad-transport-') as directory:
            work = Path(directory)
            (work / 'squad_transport_methods.inc').write_text(methods)
            exe = work / 'transport'
            # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
            for hook in ([], ['-DYOUGAME_AGENT_TEST_HOOK=1']):
                run([cxx, *hook, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-I', str(work), '-I', str(ROOT / 'port/runtime/hle'),
                     '-I', str(ROOT / 'port/runtime/host'), '-I', str(ROOT / 'port/third_party'),
                     str(ROOT / 'port/tests/squad_transport_fixture.cpp'), '-o', str(exe)])
                self.assertIn('PASS', run([str(exe)]))

    def test_agent_lineup_is_host_local_and_uses_the_players_lock_in(self):
        wire = source('port/runtime/abi/mu_squad_wire.h')
        self.assertIn('#define MU_SQUAD_OP_AGENT_LINEUP 6', wire)
        host = source('port/app/source_host.cpp')
        command = function(host, 'h_slippi_command')
        agent = command.index('payload[0] == MU_SQUAD_OP_AGENT_LINEUP')
        # Answered before any Slippi dispatch, and only by agent-test hook builds.
        self.assertLess(agent, command.index('auto& console = yougame::console::console_engine();'))
        block = command[agent:command.index('return 0;', agent)]
        self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK', block)
        self.assertIn('squad_lineup_bytes(lineup)', block)
        self.assertNotIn('squad_lineup', without_squad(host))
        hook = source('port/runtime/hle/yougame_agent_test.h')
        self.assertIn('"squadLineup"', hook)
        self.assertIn('mu_squad_side_valid', function(hook, 'squad_lineup_value'))
        online = read(GAME / 'shim/mu_squad_online.c')
        self.assertIn('agent_lineup_query();', function(online, 'gmSquad_OnlineCss'))
        self.assertIn('gmSquad_OnlineEditable()', function(online, 'gmSquad_OnlineAgentLineup'))
        select = function(online, 'gmSquad_OnlineSelect')
        # One "k:c" pair per picked fighter (all four), in pick order.
        self.assertIn('"squad: locked in %d/%d", count, GMSQUAD_SIZE', select)
        self.assertIn('for (i = 0; i < count && used > 0 && used < (int) sizeof text; ++i)', select)
        self.assertIn('"%s%u:%u", i ? "," : " ",', select)
        self.assertLess(select.index('saved_own = selection_accepted = true;'), select.index('squad: locked in'))
        css = read(GAME / 'shim/mu_squad_css.c')
        lineup = function(css, 'squad_agent_lineup')
        # The real pickers and a real START press: lock-in is the player's path, never a shortcut.
        for word in ['squad_add_member(0,', 'squad_place_coin(0, cursor,',
                     'HSD_PadCopyStatus[(u8) mnCharSel_804D6CF0].trigger |= HSD_PAD_START;']:
            self.assertIn(word, lineup)
        self.assertNotIn('gmSquad_OnlineSelect', lineup)
        self.assertIn('mnCharSel_804D6CF2 == 0', lineup)  # character select's START delay
        place = function(css, 'squad_place_coin')
        # The colour holds: DB34 resets the costume on what it takes for a new hover.
        self.assertLess(place.index('sel_icon_prev = (u8) icon;'), place.index('mnCharSel_8025DB34('))
        frame = function(css, 'mnCharSel_SquadFrame')
        self.assertLess(frame.index('squad_agent_lineup();'), frame.index('squad_buttons(p);', frame.index('squad_agent_lineup();')))
        self.assertIn('"squad: picks P%d %d/%d"', function(css, 'squad_log_picks'))

    def test_agent_test_squad_lineup_key(self):
        cxx = shutil.which('clang++') or shutil.which('c++')
        if not cxx:
            self.skipTest('needs a C++ compiler')
        with tempfile.TemporaryDirectory(prefix='squad-agent-key-') as directory:
            exe = Path(directory) / 'agent'
            includes = []
            for path in ['port/runtime/hle', 'port/runtime/host', 'port/runtime/ppc', 'port/runtime/gx',
                         'port/runtime/abi', 'port/third_party', 'port/third_party/enet/include', 'native']:
                includes += ['-I', str(ROOT / path)]
            run([cxx, '-std=c++20', '-fno-char8_t', '-O0', '-Wno-deprecated-declarations',
                 '-DMELEE_SOURCE_PORT', '-DYOUGAME_AGENT_TEST_HOOK', '-DMU_SQUAD=1', *includes,
                 str(ROOT / 'port/tests/yougame_squad_agent_test.cpp'), '-o', str(exe)])
            self.assertRegex(run([str(exe)]), r'squad agent test: \d+ checks passed')

    def test_production_game_setup_and_budget_in_both_orders(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('needs C compiler')
        with tempfile.TemporaryDirectory(prefix='squad-game-online-') as directory:
            work = Path(directory)
            (work / 'mu_squad_lineup_fixture.h').write_text(production_fixture())
            css = read(GAME / 'shim/mu_squad_css.c')
            (work / 'mu_squad_css_fixture.h').write_text(
                function(css, 'squad_xy') + function(css, 'squad_fill_buttons') +
                function(css, 'squad_buttons') + function(css, 'mnCharSel_SquadJoin'))
            online = read(GAME / 'shim/mu_squad_online.c')
            state = online[online.index('static bool online_chosen'):online.index('bool gmSquad_OnlineChosen')]
            state = state.replace('static bool online_chosen, online_ready, host_supported, selection_accepted;',
                                  'static bool host_supported, selection_accepted;')
            names = ['gmSquad_OnlineChoose', 'gmSquad_OnlineCss', 'gmSquad_OnlineSelect', 'gmSquad_OnlineTest',
                     'put32', 'budget_fact', 'console_fact', 'gmSquad_ConsoleCostumes', 'gmSquad_OnlineBudget',
                     'gmSquad_OnlineSetup']
            # The agent slices are cut at their comment text: reword one and update its marker here.
            agent = online[online.index("/* The host's agent-test lineup"):online.index('void gmSquad_OnlineCss(void)')]
            guard = online[online.index('/* The host answers the budget with 1'):online.index('extern const char* mu_ground_stage_file')]
            (work / 'mu_squad_online_fixture.h').write_text(
                'bool gmSquad_OnlineBudget(void);\n' + state + '\nstatic bool budget_ok;\n' + agent + guard +
                '\n'.join(function(online, name) for name in names))
            agent_state = css[css.index('/* Online row: the lineup the last'):css.index('static void mnCharSel_SquadReset')]
            agent_css = css[css.index("/* The online row's picks, logged when they change"):
                            css.index('/* Runs at the top of mnCharSel_Scene_OnFrame')]
            (work / 'mu_squad_agent_css_fixture.h').write_text(agent_state + agent_css)
            exe = work / 'game'
            run([cc, '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function', '-DMU_SQUAD=1',
                 '-DMU_SQUAD_ONLINE_FIXTURE=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-I', str(work), '-I', str(ROOT / 'port/runtime/abi'),
                 str(GAME / 'tests/squad_lineup.c'), '-o', str(exe)])
            result = run([str(exe)])
            self.assertIn('production game canonical exchanged setup', result)
            self.assertIn('agent-test lineup asked on each squad CSS visit', result)
            self.assertIn('agent-test lineup picked like a player', result)
            self.assertIn('cached budget', result)
            self.assertIn('costumes keep the identity and the trim for any skin pack', result)
            self.assertIn('Friends synced select budgets on the fixed floors', result)
            self.assertIn('the worst vanilla 4v4 (16590272 animation bytes) fits the 40 MiB floor on every machine', result)
            self.assertIn('Friends squads arm the disc-costume rule', result)
            print(result, end='')

    def test_budget_identity_names_the_costume_constant_version(self):
        # Costume facts now carry fixed per-fighter constants, so the identity string moved on:
        # a v2 game (local costume lengths) never shares a pool with a v3 one, and the 4-fighter squad (G3,
        # wire version 3) moved it again to v4: a 3-fighter game never shares a pool with a 4-fighter one.
        # v5 doubled the main-heap floor; v6 raised the animation floor to 0x1100000 on 40 MiB ARAM.
        text = source('port/runtime/hle/slippi_online.cpp')
        self.assertIn('"squad-budget-v6"', text)
        self.assertNotIn('"squad-budget-v5"', text)
        self.assertNotIn('"squad-budget-v4"', text)
        self.assertNotIn('"squad-budget-v3"', text)
        self.assertNotIn('"squad-budget-v2"', text)
        online = source('sourceport/game/shim/mu_squad_online.c')
        self.assertIn('gmSquad_CostumeBytes(entry, use)', online)
        self.assertNotIn('costumes_fit', online)
        self.assertIn('budget_answer(accepted)', online)

    def test_oversized_skins_load_the_disc_copy_in_squad_matches(self):
        # The host records the costume facts and, while an online squad match is set up, opens the
        # standard copy for a costume or hat over its fixed size; only a standard copy over refuses.
        host = source('port/runtime/hle/slippi_online.cpp')
        self.assertIn('g_squad_costumes.record(p, n);', host)
        self.assertIn("q.assign(1, g_squad_budget.size() != 64 ? 0 : over.empty() ? 1 : mod ? 3 : 2);", host)
        # Armed only by op 5's squad setup (never by SquadSetup alone, which StartSlippiGame clears
        # on frame 1), after the copies are checked again in the match's view; cleared by op 0, a
        # normal or refused op 5, every connection cleanup, a dropped client and shutdown.
        command = function(host, 'squad_command')
        op5 = command[command.index('case 5: {'):command.index('default:')]
        self.assertLess(op5.index('g_squad_costume_match = false;'), op5.index('g_netplay->SquadSetup(&setup)'))
        self.assertLess(op5.index('squad_standards(&standards, &over, &mod)'), op5.index('g_squad_costume_match = g_squad_standard != nullptr;'))
        self.assertLess(op5.index('g_netplay->RefuseSquad(squad_costume_refusal(mod)'), op5.index('g_squad_costume_match = g_squad_standard != nullptr;'))
        self.assertIn('g_squad_costume_match = false;', command[command.index('case 0:'):command.index('case 1:')])
        self.assertIn('g_squad_costume_match = false;', function(host, 'cleanup_connection'))
        self.assertIn('g_squad_costume_match = false;', function(host, 'shutdown'))
        dropped = host[host.index('    g_netplay = nullptr;\n#if MU_SQUAD'):]
        self.assertLess(dropped.index('g_squad_costume_match = false;'), dropped.index('#endif'))
        rule = function(host, 'squad_costume_rule')
        self.assertIn('if (!g_squad_costume_match) {\n    if (!console) return false;\n    squad_console_forget();\n'
                      '    return g_squad_console.rule(path, &out->budget, &out->standard);\n  }', rule)
        self.assertNotIn('SquadSetup', rule)
        app = source('port/app/source_host.cpp')
        lookup = app[app.index('int32_t h_disc_entrynum(const char* path) {'):app.index('int32_t resolve_disc_entrynum(const std::string& p) {')]
        self.assertIn('const int32_t opened = resolve_disc_entrynum(p);', lookup)
        self.assertIn('slippi::online::squad_costume_rule(p, &costume, yougame::console::console_active())', lookup)
        self.assertIn('rule::pick(costume.budget, file, costume.standard.file)', lookup)
        self.assertIn('if (slippi::online::squad_costume_first_fallback(p))', lookup)
        self.assertIn('squad: skin %s over its budget, vanilla used this match', lookup)
        self.assertIn('slippi::online::set_squad_standard_copy(squad_standard_copy);', app)
        self.assertIn('g_disc_paths = g_paths;', app)
        # The game: 3 names the mod's costumes; offline charges costumes at their local length.
        online = source('sourceport/game/shim/mu_squad_online.c')
        self.assertIn("This mod's costumes are too big for Squad Strike online. Search refused.", online)
        budget = function(source('sourceport/game/shim/mu_squad.c'), 'squad_budget_file')
        self.assertIn('(use == GMSQUAD_FILE_COSTUME || use == GMSQUAD_FILE_HAT) && squad_fixed_budget()', budget)
        # Online and Friends' synced select (a console session) decide on the same fixed facts.
        fixed = function(source('sourceport/game/shim/mu_squad.c'), 'squad_fixed_budget')
        self.assertIn('return gmSquad_OnlineChosen() || gmSquad_LobbyChosen() || mu_console_active();', fixed)
        engine = function(source('sourceport/game/shim/mu_squad.c'), 'gmSquad_Budget')
        self.assertIn('const u32 allm = fixed ? MU_SQUAD_ONLINE_ALLM : s_allm_budget;', engine)
        self.assertIn('const u32 alla = fixed ? MU_SQUAD_ONLINE_ALLA : s_alla_budget;', engine)
        self.assertNotIn('local > bytes', budget)
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('needs a C++ compiler')
        with tempfile.TemporaryDirectory(prefix='squad-costume-') as directory:
            exe = Path(directory) / 'rule'
            run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-I', str(ROOT / 'port/runtime/hle'),
                 str(ROOT / 'port/tests/squad_costume_rule_test.cpp'), '-o', str(exe)])
            self.assertIn('squad costume rule: PASS', run([str(exe)]))

    def test_cmake_define_reaches_runtime_libraries_and_consumers(self):
        cmake = shutil.which('cmake')
        if not cmake:
            self.skipTest('needs CMake')
        text = source('port/CMakeLists.txt')
        # Discover every runtime source glob and its consuming library, including Linux.
        targets = []
        for path in ['port/CMakeLists.txt', 'port/linux/full/CMakeLists.txt']:
            cm = source(path)
            variables = re.findall(r'file\(GLOB\s+(\w+)[^)]*runtime/hle/\*\.cpp[^)]*\)', cm)
            self.assertTrue(variables, path)
            for variable in variables:
                consumers = re.findall(r'add_library\((\w+)\s+STATIC\s+\$\{' + variable + r'\}', cm)
                self.assertTrue(consumers, variable)
                for target in consumers:
                    self.assertIn(f'mu_squad_target({target})', cm, path)
                    targets.append(target)
        self.assertEqual(set(targets), {'runtime', 'runtime_ns', 'full_runtime'})
        mac = source('port/mac/build_full.py')
        self.assertIn("os.environ.get('MELEE_SQUAD_STRIKE', 'OFF')", mac)
        self.assertIn("if args.squad_strike:\n        cpp += ['-DMU_SQUAD=1']", mac)
        self.assertIn("if args.squad_strike and source.suffix == '.mm':", mac)
        self.assertLess(mac.index("if source.name == 'embed_mac.mm':"),
                        mac.index("if args.squad_strike and source.suffix == '.mm':"))
        self.assertIn("else cflags if source.suffix == '.c' else cpp", mac)
        # Squad hosts build in their own directory and say so in the manifest.
        self.assertIn("('-squad' if squad else '')", mac)
        self.assertIn("BUILD = build_dir(args.agent_test_hook, args.squad_strike)", mac)
        self.assertIn("'squad_strike':args.squad_strike,", mac)
        with tempfile.TemporaryDirectory(prefix='squad-cmake-') as directory:
            work = Path(directory)
            (work / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.15)
project(squad_define CXX)
include("{ROOT / 'port/cmake/squad.cmake'}")
foreach(target runtime runtime_ns full_runtime)
 add_library(${{target}} STATIC flag.cpp)
 mu_squad_target(${{target}})
 add_executable(${{target}}_consumer flag.cpp)
 target_link_libraries(${{target}}_consumer PRIVATE ${{target}})
endforeach()
''')
            (work / 'flag.cpp').write_text('int main() { return 0; }\n')
            for enabled in ['OFF', 'ON']:
                build = work / enabled
                run([cmake, '-S', str(work), '-B', str(build), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                     '-DMELEE_SQUAD_STRIKE=' + enabled])
                commands = json.loads((build / 'compile_commands.json').read_text())
                self.assertEqual(len(commands), 6)
                for entry in commands:
                    self.assertEqual('MU_SQUAD=1' in entry['command'], enabled == 'ON')

    def test_noslippi_combined_build_has_no_transport_and_real_cpu_flags(self):
        disabled = source('sourceport/game/shim/mu_squad_online.c').split('#else\n', 1)[1]
        self.assertNotIn('mu_slippi_cmd(', disabled)
        self.assertIn('gmSquad_OnlineChosen(void) { return false; }', disabled)
        stub = source('sourceport/game/shim/mu_noslippi.c')
        self.assertEqual(function(stub, 'mu_replay_fighter_flags'),
                         function(source('sourceport/game/shim/mu_replay.c'), 'mu_replay_fighter_flags'))
        self.assertIn('if (got) *got = 0;', function(stub, 'mu_slippi_cmd'))
        self.assertIn('return -1;', function(stub, 'mu_slippi_cmd'))
        self.assertIn('mu_slippi_title_active(void) { return 0; }', stub)

    def test_search_and_lock_in_fail_closed_with_game_drawn_error(self):
        menu = source('sourceport/game/shim/mu_slippi_menu.c')
        search = function(menu, 'mu_slippi_find_opponent')
        self.assertLess(search.index('!gmSquad_OnlineSearchReady()'), search.index('mu_slippi_cmd(CMD_FIND_OPPONENT'))
        sss = function(source('sourceport/game/shim/mu_slippi_sss.c'), 'mu_slippi_sss_lock_in')
        self.assertLess(sss.index('if (!gmSquad_OnlineSelect(sel.char_id, sel.char_color)) return 0;'),
                        sss.index('mu_slippi_set_selections(&sel)'))
        css = source('sourceport/game/shim/mu_slippi_css.c')
        # A refused lock-in starts no search (G3 review 5): both lock-in-then-search paths check it.
        self.assertIn('if (mu_slippi_sss_lock_in(sb)) mu_slippi_sss_find_match();', function(css, 'lock_in_and_search'))
        self.assertIn('if (mu_slippi_sss_lock_in(stage_behavior)) mu_slippi_sss_find_match();',
                      function(source('sourceport/game/shim/mu_slippi_sss.c'), 'mu_slippi_sss_run_callback'))
        self.assertIn('strncpy((char*) buf, gmSquad_OnlineError(), 241)', css)
        online = source('sourceport/game/shim/mu_squad_online.c')
        self.assertIn('squad: host has no squad support, search refused', online)
        for name in ['gmSquad_OnlineChoose', 'gmSquad_OnlineSelect']:
            self.assertIn('got == 1' if name.endswith('Choose') else 'got != 1', function(online, name))
        gate = function(online, 'gmSquad_OnlineSearchReady')
        self.assertIn('host_supported && selection_accepted && budget_accepted', gate)
        self.assertIn('gmSquad_OnlineBudget();', function(online, 'gmSquad_OnlineCss'))
        budget = function(online, 'gmSquad_OnlineBudget')
        self.assertLess(budget.index('if (budget_scanned)'), budget.index('for (ck ='))
        self.assertIn('put32(payload + 1, MU_SQUAD_ONLINE_ALLM)', budget)
        self.assertIn('put32(payload + 5, MU_SQUAD_ONLINE_ALLA)', budget)
        self.assertNotIn('put32(payload + 1, main_bytes)', budget)
        host = function(source('port/runtime/hle/slippi_online.cpp'), 'squad_command')
        self.assertLess(host.index('g_squad_side = {}'), host.index('command_payload_valid'))
        dispatch = function(source('port/runtime/hle/slippi_online.cpp'), 'handle_command')
        self.assertLess(dispatch.index('cmd == MU_SQUAD_COMMAND'), dispatch.index('valid_command_payload'))

    def test_one_key_builder_and_explicit_peer_abort(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        find = function(link, 'find_match')
        self.assertIn('compatibility(search.mode, squad_row, search.squad_budget)', find)
        self.assertNotIn('compatibility_key(', find)
        self.assertNotIn('pool_tag(', find)
        shared = function(link, 'compatibility')
        self.assertIn('ranked_rules_digest(mode)', shared)
        self.assertIn('squad::compatibility(', shared)
        net = source('port/runtime/hle/slippi_net.cpp')
        self.assertEqual(net.count('SquadPeerDisconnect(ev.data)'), 2) # connecting and connected
        disconnect = function(net, 'NetplayClient::Disconnect')
        self.assertIn('enet_peer_disconnect(peer.first, pending_disconnect_reason_', disconnect)
        cancel = function(net, 'NetplayClient::CancelSquad')
        self.assertIn('ForceDisconnect(DisconnectReason::SQUAD_PROTOCOL)', cancel)
        receive = function(net, 'NetplayClient::SquadPeerDisconnect')
        self.assertIn('peer cancelled protocol error', receive)
        selection = function(net, 'NetplayClient::SetMatchSelections')
        self.assertEqual(selection.count('CancelSquad('), 2)
        # A peer's squad cancel while connecting shows the squad error instead of searching again.
        retry = net.index('slippi: connection attempt failed, searching again')
        guard = net.rindex('client->SquadCancelled()', 0, retry)
        self.assertIn('fail(client->SquadCancelText()); return;', net[guard:retry])
        self.assertNotIn('state_ = INITIALIZING', net[guard:retry])
        # Both on-screen paths show why the squad match was refused, never the old generic literal.
        online = source('port/runtime/hle/slippi_online.cpp')
        # The reason is read before cleanup_connection() drops g_netplay.
        self.assertRegex(online, r'std::string reason = g_netplay->SquadCancelText\(\);\s*cleanup_connection\(\);\s*g_forced_error = std::move\(reason\);')
        for text in [net, online]:
            self.assertNotIn('Squad Strike protocol error: invalid or missing lineup', text)
        header = source('port/runtime/hle/slippi_net.h')
        self.assertIn('const char* SquadCancelText() const { return squad_cancel_text_.load(); }', header)
        self.assertRegex(header, r'std::atomic<const char\*> squad_cancel_text_\{"[^"]+"\};')

    def test_opponent_leaving_before_the_first_frame_cancels_to_the_css(self):
        # Host: a locked-in squad connection that drops before the first frame says why and
        # starts nothing; without Squad Strike (normal Slippi, Doubles) the branch is unchanged.
        online = source('port/runtime/hle/slippi_online.cpp')
        prep = function(online, 'prepare_online_match_state')
        at = prep.index('const bool squad_cancelled')
        branch = prep[at:prep.index('prepare_online_match_state(q);', at)]
        self.assertIn('g_last_search.squad && g_local_selections.is_character_selected && !g_in_online_match',
                      branch)
        self.assertLess(branch.index('cleanup_connection();'),
                        branch.index('g_forced_error = "Match cancelled: connection lost.";'))
        self.assertIn('squad: connection lost before the first frame, match cancelled', branch)
        self.assertNotIn('Match cancelled', without_squad(prep))
        self.assertRegex(without_squad(prep), r'\} else \{\s*cleanup_connection\(\);\s*prepare_online_match_state\(q\);')
        # Game: the CSS exit and the splash go back to the CSS (which restores the lineup) while
        # the host does not report the connection, before any splash, preload or VS start.
        menu = source('sourceport/game/shim/mu_slippi_menu.c')
        exit_css = function(menu, 'css_decide')
        self.assertLess(exit_css.index('squad_opponent_gone("CSS exit")'), exit_css.index('mu_slippi_splash_init();'))
        self.assertIn('gmSquad_OnlineCss();', function(menu, 'css_prep'))
        gone = function(menu, 'squad_opponent_gone')
        splash = function(menu, 'splash_decide')
        self.assertNotIn('squad_opponent_gone', without_squad(splash) + without_squad(exit_css))
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('needs C compiler')
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int logged;
static void OSReport(const char* format, ...) { logged += strstr(format, "back to the CSS") != NULL; }
typedef struct GameModeState GameModeState;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT, MU_SLP_MODE_TEAMS };
enum { MU_SLP_MM_IDLE, MU_SLP_MM_CONNECTION_SUCCESS = 4, MU_SLP_MM_ERROR = 5 };
enum { MU_SLP_STATE_CSS, MU_SLP_STATE_VS = 2 };
static struct { int mode; } slp;
static struct { int connection_state; } last_match;
static int chosen, load_fails, reported, next = -1;
static int mu_slippi_load_match_state(void* out) { (void) out; last_match.connection_state = reported; return load_fails ? -1 : 0; }
static int chosen_friend, chosen_lobby;
static int gmSquad_LobbyChosen(void) { return chosen_lobby; }
static int gmSquad_OnlineChosen(void) { return chosen; }
/* As mu_squad_online.c: Unranked's squad search, or Squad Strike in a friend room (Direct). */
static int gmSquad_OnlineLive(int mode)
{
    return chosen && (chosen_friend ? mode == MU_SLP_MODE_DIRECT : mode == MU_SLP_MODE_UNRANKED);
}
static void gm_SetNextGameModeStateId(int id) { next = id; }
#if MU_SQUAD
GONE
#endif
SPLASH
static int after_room(int squad, int friends, int mode, int state, int fails)
{
    chosen = squad; chosen_friend = friends; slp.mode = mode; reported = state; load_fails = fails; next = -1;
    splash_decide(0);
    return next;
}
static int after(int squad, int mode, int state, int fails)
{
    chosen = squad; chosen_friend = 0; slp.mode = mode; reported = state; load_fails = fails; next = -1;
    splash_decide(0);
    return next;
}
int main(void)
{
    int mode, state;
    for (mode = MU_SLP_MODE_RANKED; mode <= MU_SLP_MODE_TEAMS; ++mode)
        for (state = MU_SLP_MM_IDLE; state <= MU_SLP_MM_ERROR; ++state) {
            /* Normal Slippi and Doubles: the splash always goes on to VS, as before. */
            assert(after(0, mode, state, 0) == MU_SLP_STATE_VS);
            assert(after(0, mode, state, 1) == MU_SLP_STATE_VS);
#if MU_SQUAD
            if (mode == MU_SLP_MODE_UNRANKED)
                assert(after(1, mode, state, 0) ==
                       (state == MU_SLP_MM_CONNECTION_SUCCESS ? MU_SLP_STATE_VS : MU_SLP_STATE_CSS));
            else
                assert(after(1, mode, state, 0) == MU_SLP_STATE_VS);
            /* A friend room's Squad Strike runs in Direct and cancels the same way. */
            if (mode == MU_SLP_MODE_DIRECT)
                assert(after_room(1, 1, mode, state, 0) ==
                       (state == MU_SLP_MM_CONNECTION_SUCCESS ? MU_SLP_STATE_VS : MU_SLP_STATE_CSS));
            else
                assert(after_room(1, 1, mode, state, 0) == MU_SLP_STATE_VS);
#endif
        }
#if MU_SQUAD
    assert(after(1, MU_SLP_MODE_UNRANKED, MU_SLP_MM_CONNECTION_SUCCESS, 1) == MU_SLP_STATE_CSS);
    assert(logged == 5 + 5 + 1);
    chosen_lobby = 1;
    assert(after(0, MU_SLP_MODE_DIRECT, MU_SLP_MM_ERROR, 0) == MU_SLP_STATE_CSS);
    chosen_lobby = 0;
    assert(logged == 12); /* every state but connected (Unranked, then the friend room), then the failed read */
#else
    assert(!logged);
#endif
    puts("PASS: squad splash cancels to the CSS without the connection; other modes unchanged");
    return 0;
}
'''.replace('GONE', gone).replace('SPLASH', splash)
        with tempfile.TemporaryDirectory(prefix='squad-cancel-') as directory:
            work = Path(directory)
            (work / 'cancel.c').write_text(harness)
            for flag in ['1', '0']:
                exe = work / ('cancel' + flag)
                run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-DMU_SQUAD=' + flag,
                     '-fsanitize=address,undefined', str(work / 'cancel.c'), '-o', str(exe)])
                self.assertIn('PASS', run([str(exe)]))

    def test_start_short_of_four_says_why_before_the_ready_flag_return(self):
        css = source('sourceport/game/shim/mu_slippi_css.c')
        inputs = function(css, 'mu_slippi_css_inputs')
        # Idle: the refusal is explained before the vanilla ready flag (cleared while the coin is
        # back in the hand at 3/4) ends the START press.
        idle = inputs[inputs.index('case MU_SLP_MM_IDLE:'):inputs.index('case MU_SLP_MM_INITIALIZING:')]
        self.assertRegex(idle, r'if \(\*char_chosen == 0\) \{\s*#if MU_SQUAD\s*squad_start_refused\(\);\s*#endif\s*return 0;')
        # Connected and not locked in: the same line on START.
        connected = inputs[inputs.index('if (m->local_ready == 0) {'):]
        self.assertLess(connected.index('squad_start_refused();'),
                        connected.index('if (advance && *char_chosen != 0 && gm_801A4BB8() != 0) {'))
        self.assertNotIn('squad_start_refused', without_squad(inputs))
        refused = function(css, 'squad_start_refused')
        self.assertIn('if (!gmSquad_OnlineLive(mu_slippi_state()->mode)) return;', refused)
        self.assertIn('gmSquad_OnlineNeedLineup(count);', refused)
        self.assertIn('lbAudioAx_80024030(3);', refused)
        self.assertIn('gmSquad_OnlinePickCount(gmSquad_CssPreview(&ck, &color, lineup));', function(css, 'text_think'))
        online = source('sourceport/game/shim/mu_squad_online.c')
        need = function(online, 'gmSquad_OnlineNeedLineup')
        self.assertIn('"Pick %d fighters first (%d/%d).", GMSQUAD_SIZE, count, GMSQUAD_SIZE', need)
        self.assertIn('OSReport("squad: lock-in refused, %d/%d fighters picked\\n", count, GMSQUAD_SIZE);', need)
        self.assertIn('gmSquad_OnlineNeedLineup(count);', function(online, 'gmSquad_OnlineSelect'))
        self.assertNotIn('NeedThree', online + css)

    def test_online_picks_show_as_icons_under_the_tag_and_used_fighters_are_marked(self):
        css = source('sourceport/game/shim/mu_squad_css.c')
        # The online CSS is the 1P layout: the row is placed from the 1P tag box joint and the tag
        # text, never the VS name joint alone (which put build 75's icons off screen).
        spot = function(css, 'squad_online_row_spot')
        self.assertIn('mnCharSel_804D6CF5 == 1 ? mnCharSel_803F0EBC.tag_box_joint', spot)
        self.assertIn('tag->text->box_size_y * tag->text->font_size.y', spot)
        row = function(css, 'squad_icon_row')
        online = row[row.index('if (gmSquad_OnlineChosen()) {\n        /* Pick order'):row.index('/* The row sits just above')]
        self.assertIn('squad_online_row_spot(&cx, &cy, &cz)', online)
        self.assertIn('squad_icons_shown[p] = 0;', online)
        # A player's log tells an icon load failure from a row that could not be placed.
        self.assertIn('OSReport("squad: stock icons loaded\\n");', function(css, 'squad_icons_load'))
        self.assertIn('OSReport("squad: name tag not built yet, picks shown as text for now\\n");', online)
        self.assertRegex(online, r'squad_icon_create\([^;]*, true\);')
        self.assertIn('squad_slot_places(cks, n, x0, squad_icon_gap, cy + squad_icon_dy);', online)
        # Empty places read as four: one numbered square per empty slot, hidden once filled.
        places = function(css, 'squad_slot_places')
        self.assertIn("i >= n || cks[i] != GMSQUAD_NONE || squad_canvas < 0", places)
        self.assertIn('"%d", i + 1', places)
        # Bottom line: empty while icons draw; a refusal or the lineup text as the fallback.
        banners = function(css, 'squad_banners')
        live = banners[:banners.index('if (squad_canvas < 0 || mnCharSel_804D6CF5 == 1)')]
        self.assertRegex(live, r"if \(squad_icon_row\(0, NULL\) && !squad_note\[0\]\) line\[0\] = '\\0';\s*"
                               r'else squad_line\(0, line, sizeof line\);\s*squad_grid_marks\(0\);')
        self.assertNotIn('squad_hint_line', css)
        self.assertNotIn('B: undo', css)
        # Used fighters: a mark per pick over its portrait, keyed by the lineup so undo and clear
        # take marks back; online only.
        marks = function(css, 'squad_grid_marks')
        self.assertIn('gmSquad_ModeAllowed() && (gmSquad_OnlineChosen() || gmSquad_LobbyChosen())', marks)
        self.assertIn('gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), order)', marks)
        self.assertIn('if (key == squad_marks_shown) return;', marks)
        self.assertIn('text->hidden = 1;', marks)
        self.assertIn('icons[icon].bound_l', marks)
        self.assertIn('squad_marks_shown = 0;', function(css, 'mnCharSel_SquadJoin'))
        # A on a fighter already picked buzzes and eats the press; drawing never reaches the wire.
        fill = function(css, 'squad_fill_buttons')
        used = fill[fill.index('const int ck = squad_hovered(cursor);'):]
        used = used[:used.index('return;') + len('return;')]
        self.assertIn('gmSquad_rows[port][i].ckind == (u8) ck', used)
        self.assertIn('HSD_PadCopyStatus[pad].trigger &= ~HSD_PAD_A;', used)
        self.assertIn('lbAudioAx_80024030(3);', used)
        for name in ('squad_grid_marks', 'squad_slot_places', 'squad_online_row_spot'):
            self.assertNotRegex(source('sourceport/game/shim/mu_squad_online.c'), name)

    def test_trim_notice_is_shown_on_the_splash(self):
        splash = source('sourceport/game/shim/mu_slippi_splash.c')
        self.assertIn(CREDIT, splash)
        text = function(splash, 'mu_slippi_splash_text')
        self.assertIn('gmSquad_OnlineReady() && gmSquad_OnlineTrimNotice()[0]', text)
        self.assertLess(text.index('gmSquad_OnlineTrimNotice()'), text.index('/* Stage name */'))
        self.assertNotRegex(without_squad(splash), r'gmSquad|mu_squad')
        setup = function(source('sourceport/game/shim/mu_squad_online.c'), 'gmSquad_OnlineSetup')
        self.assertLess(setup.index('trim_text[0] = 0;'), setup.index('gmSquad_TrimOnline(start);'))
        self.assertLess(setup.index('gmSquad_TrimOnline(start);'), setup.index('OSReport("squad: %s\\n", trim_text);'))

    def test_existing_row_indices_and_no_slippi_row_absent(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('needs C compiler')
        code = r'''#include <assert.h>
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT,
 MU_SLP_MODE_TEAMS, MU_SLP_MODE_PARTY, MU_SLP_OPT_LOGIN, MU_SLP_OPT_LOGOUT,
 MU_SLP_OPT_UPDATE, MU_SLP_OPT_FRIENDS, MU_SLP_OPT_SLIPPI, MU_SLP_OPT_TOURNAMENT
#ifdef MELEE_DOUBLES
 , MU_SLP_OPT_DOUBLES
#endif
 };
#include "mu_online_menu.h"
#ifdef MELEE_DOUBLES
#define DBL 1
#else
#define DBL 0
#endif
int main(void) {
 for (int tourney=0; tourney<2; ++tourney) {
  tournament_offered=tourney;
  assert(mu_online_menu_option(1,0,0)==MU_SLP_MODE_RANKED);
  assert(mu_online_menu_option(1,0,1)==MU_SLP_MODE_UNRANKED);
  assert(mu_online_menu_row(1,0,MU_SLP_OPT_FRIENDS)==2);
  assert(mu_online_menu_row(1,0,MU_SLP_OPT_SLIPPI)==3+DBL+tourney);
  if(tourney) assert(mu_online_menu_row(1,0,MU_SLP_OPT_TOURNAMENT)==3+DBL);
#ifdef MELEE_DOUBLES
  assert(mu_online_menu_option(1,0,3)==MU_SLP_OPT_DOUBLES);
#endif
#if MU_SQUAD && !defined(MU_NO_SLIPPI)
  /* Its own option: the last row opens Squad Strike, never a second Doubles. */
  assert(mu_online_menu_option(1,0,4+DBL+tourney)==MU_SLP_OPT_SQUAD);
  assert(mu_online_menu_row(1,0,MU_SLP_OPT_SQUAD)==4+DBL+tourney);
  assert(mu_online_menu_count(1,0)==5+DBL+tourney);
#else
  assert(mu_online_menu_count(1,0)==4+DBL+tourney);
#endif
  for(int row=0;row<3;++row) assert(mu_online_menu_option(1,1,row)==row+2);
  for(int row=0;row<8;++row) assert(mu_online_menu_option(0,0,row)==row);
 }
 return 0;
}'''
        with tempfile.TemporaryDirectory(prefix='squad-rows-') as directory:
            work = Path(directory)
            (work / 'rows.c').write_text(code)
            for flags in [[], ['-DMU_SQUAD=1'], ['-DMU_SQUAD=1', '-DMU_NO_SLIPPI'], ['-DMELEE_DOUBLES'],
                          ['-DMELEE_DOUBLES', '-DMU_SQUAD=1']]:
                run([cc, '-std=c11', *flags, '-I', str(GAME / 'include'),
                     str(work / 'rows.c'), '-o', str(work / 'rows')])
                run([str(work / 'rows')])

    @NEEDS_DECOMP
    def test_coin_port_matches_picker_for_all_four_controllers(self):
        menu = source('sourceport/game/shim/mu_slippi_menu.c')
        self.assertIn('vs.unk_530.x6 = gm_801677F0()', function(menu, 'online_on_load'))
        self.assertIn('gm_801B06B0(css, 0xE, ev->x2, 0, ev->x3, ev->nametag, 0, ev->x6)',
                      function(menu, 'css_prep'))
        setup = function(read(DECOMP / 'gm/gm_1B03.c'), 'gm_801B06B0')
        self.assertIn('css_data->unk_0x0 = slot + 1', setup)
        picker = read(DECOMP / 'mn/mncharsel.c')
        self.assertIn('mnCharSel_804D6CF0 = mnCharSel_804D6CB0->unk_0x0 - 1', picker)
        self.assertIn('gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : door',
                      read(GAME / 'shim/mu_squad_css.c'))
        self.assertIn('u8 port = gm_801677F0()', function(read(GAME / 'shim/mu_slippi_sss.c'), 'mu_slippi_sss_lock_in'))


if __name__ == '__main__':
    unittest.main()
