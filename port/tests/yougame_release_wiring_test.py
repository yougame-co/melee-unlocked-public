"""Update-only port from 589c907: offline source checks, no service requests."""
from pathlib import Path
import unittest
import re
from relay_pads_wiring_test import code, block
def literal_function(text, name):
    # Locate braces in masked C++, but keep string literals in the returned body.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m.group()), text, flags=re.S)
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', masked)
    if not match: raise AssertionError('Missing function: ' + name)
    body = block(masked, match.end())
    return text[match.end():match.end() + len(body)]
ROOT = Path(__file__).resolve().parents[2]
def source(p): return (ROOT / p).read_text(encoding='utf-8-sig')
class HeadReleaseWiringTest(unittest.TestCase):
    def test_refusal_precedes_generic_error(self):
        bridge = literal_function(source('port/runtime/hle/yougame_link.cpp'), 'bridge')
        self.assertLess(bridge.index('update::required(refused)'), bridge.index('if (!j.value("ok", false))'))
        self.assertIn('throw update::Required();', bridge)
        self.assertIn('if (cancelled && cancelled())', bridge)
    def test_ticket_and_recovery_stop_without_retry(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        find = literal_function(link, 'find_match')
        self.assertIn('catch (const update::Required&)', find)
        self.assertEqual(find.count('while (!cancelled() && !update_refused && now_ms() < until)'), 2)
        self.assertEqual(find.count('if (update_refused) return fail(update::kMessage, false, true);'), 2)
        self.assertIn('catch (const update::Required&) { update_refused = true; throw; }', find)
        self.assertIn('bool retry = false', link)
        self.assertIn('m.retry = retry && !update_required;', literal_function(link, 'fail'))
        self.assertIn('return fail(update::kMessage, false, true);', find)
        refused = block(literal_function(link, 'fail').split('if (update_required) {', 1)[1], 0)
        self.assertIn('set_status(update::kMessage);', refused)
        self.assertIn('return m;', refused)
        self.assertIn('catch (const update::Required&) { cancel_open_ = {}; throw; }', literal_function(link, 'open_session'))
    def test_queue_and_room_refusal_before_assignment(self):
        find = literal_function(source('port/runtime/hle/yougame_link.cpp'), 'find_match')
        self.assertEqual(find.count('if (update::required(refused))'), 2)
        self.assertLess(find.index('if (update::required(refused))'), find.index('if (t == "matched"'))
        self.assertIn('yougame: update required (current %s)', find)
    def test_notice_processed_before_window_and_persistent(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        reader = literal_function(link, 'handle_app_line')
        self.assertLess(reader.index('g_update_notice.command(line)'), reader.index('#ifdef MELEE_LINUX_X11_EMBED'))
        self.assertIn('return g_update_notice.available() ? update::kMessage : g_status;', literal_function(link, 'status_line'))
        notice = literal_function(source('port/runtime/hle/yougame_update.h'), 'command')
        for forbidden in ('cancel(', 'bridge(', 'stop(', 'host::'): self.assertNotIn(forbidden, notice)
    def test_online_screens_reuse_status_area(self):
        ui = source('port/runtime/gx/pc_settings.cpp')
        area = ui[ui.index('const bool show_status ='):ui.index('ImGuiStyle launcher_saved_style;')]
        for text in ('!slippi::online::is_online_match()', '&& show_status',
                     'status == yougame::update::kMessage', 'ImGui::Begin("##yougame_friend_waiting"',
                     'ImGui::TextUnformatted(status.c_str())'):
            self.assertIn(text, area)
    def test_visibility_is_independent_of_friends(self):
        think = literal_function(source('sourceport/game/shim/mu_slippi_menu.c'), 'mu_slippi_online_menu_think')
        self.assertIn('int online_visible = 1;', think)
        self.assertEqual(think.count('online_visible = 0;'), 2)
        self.assertIn('if (online_visible) friends_menu |= 4;', think)
        online = source('port/runtime/hle/slippi_online.cpp')
        self.assertIn('g_online_menu_visible = (payload[0] & 4) != 0;', online)
        self.assertIn('(payload[0] & 3) != 0, (payload[0] & 3) == 2', online)
        self.assertIn('inline bool online_status_visible()', source('port/runtime/host/netplay_state.h'))
    def test_main_pool_default_and_granted_direct_port(self):
        self.assertIn('set(YOUGAME_COMPAT_TAG "" CACHE STRING', source('CMakeLists.txt'))
        self.assertIn('yougame_blend.h', source('port/runtime/hle/yougame_link.cpp'))
        self.assertIn('slippi_login && !agent_script ? MatchmakingRoute::Slippi : MatchmakingRoute::Locked', source('port/runtime/hle/yougame_mode_policy.h'))



"""Release wiring, source-only: no game, login file, or external server is touched."""
from pathlib import Path
import unittest

from relay_pads_wiring_test import code, function, block

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class ReleaseWiringTest(unittest.TestCase):
    def test_cache_define_reaches_all_game_runtimes(self):
        root = source("CMakeLists.txt")
        self.assertIn('set(YOUGAME_COMPAT_TAG "" CACHE STRING', root)
        windows = source("port/CMakeLists.txt")
        self.assertIn('target_compile_definitions(runtime PRIVATE YOUGAME_COMPAT_TAG="${YOUGAME_COMPAT_TAG}"', windows)
        self.assertIn('MELEE_NO_SLIPPI YOUGAME_COMPAT_TAG="${YOUGAME_COMPAT_TAG}"', windows)
        self.assertIn('PRIVATE YOUGAME_COMPAT_TAG="${YOUGAME_COMPAT_TAG}"', source("port/linux/full/CMakeLists.txt"))

    def test_all_outgoing_lobbies_share_the_tagged_key(self):
        raw = source("port/runtime/hle/yougame_link.cpp")
        find = function(code(raw), "find_match")
        self.assertIn('const std::string compat = compatibility(search.mode);', find)
        self.assertIn('{"compatibility", compat}', raw)
        self.assertIn('friends::team_ticket(compat, team_room)', find)
        self.assertIn('friends::request_ticket(compat, search.friend_room, request,', find)
        friends = source("port/runtime/hle/yougame_friends.h")
        self.assertEqual(friends.count('{"compatibility", compatibility}'), 2)
        key = source("port/runtime/hle/yougame_compatibility.h")
        self.assertIn('const std::string& tag = compat_tag', key)
        self.assertLess(key.index('suffix += "-" + tag'), key.index('key.substr(0, 64 - suffix.size())'))
        self.assertIn('false, rules_digest, tag)', key)

    def test_branch_log_once_after_log_selection(self):
        link = code(source("port/runtime/hle/yougame_link.cpp"))
        ready = function(link, "logging_ready")
        self.assertIn('std::call_once(once, [] {', ready)
        self.assertIn('if (yougame::active() && *compat_tag)', ready)
        self.assertIn('branch build \\"%s\\": matches only the same branch', source("port/runtime/hle/yougame_link.cpp"))
        main = code(source("port/app/main.cpp"))
        self.assertLess(main.index('host::options.log_file = argv[++i]'), main.index('yougame::relay_pads::logging_ready();'))

    def test_update_code_precedes_busy_retry_and_failure_text(self):
        link = code(source("port/runtime/hle/yougame_link.cpp"))
        bridge = link[link.index("json bridge("):link.index("class Socket")]
        self.assertLess(bridge.index('update::required(refused)'), bridge.index('retry.delay(j)'))
        self.assertIn('throw update::Required();', bridge)
        self.assertLess(bridge.index('update::required(refused)'), bridge.index('if (!j.value('))
        self.assertIn('if (!search_stopped())', bridge)

    def test_refusal_is_typed_and_never_retries(self):
        link = code(source("port/runtime/hle/yougame_link.cpp"))
        fail = function(link, "fail")
        self.assertIn('m.retry = retry && !update_required;', fail)
        self.assertIn('m.update_required = update_required;', fail)
        self.assertIn('if (update_required)', fail)
        find = function(link, "find_match")
        self.assertIn('catch (const update::Required&)', find)
        self.assertGreaterEqual(find.count('return fail(update::kMessage, false, true);'), 4)
        self.assertIn('catch (const update::Required&) { update_refused = true; throw; }', find)
        self.assertEqual(find.count('while (!cancelled() && !update_refused && now_ms() < until)'), 2)
        self.assertIn('return !update_refused && now_ms() < until && queue.open_session', find)

    def test_queue_update_code_precedes_match_assignment(self):
        find = function(code(source("port/runtime/hle/yougame_link.cpp")), "find_match")
        queue = find[find.index('if (queued) {'):]
        self.assertLess(queue.index('if (update::required(refused))'), queue.index('if (t =='))

    def test_update_refusal_stops_both_blend_sides(self):
        blend = function(code(source("port/runtime/hle/slippi_net.cpp")), "blendMatchmaking")
        self.assertIn('match.update_required = coord.live(Side::YouGame);', blend)
        self.assertIn('yg.update_required = true;', blend)
        publish = blend[blend.index('if (match.update_required) {'):blend.index('return match;')]
        self.assertLess(publish.index('std::lock_guard<std::mutex> lk(mutex);'), publish.index('coord.stop();'))
        self.assertLess(publish.index('coord.stop();'), publish.index('yg.update_required = true;'))
        self.assertIn('return yg.update_required || !searching() || !live(Side::Slippi);', blend)
        at = blend.index('if (yg.update_required) {')
        refused = block(blend, at + len('if (yg.update_required) {'))
        for action in ('coord.stop();', 'dropSlippi();', 'fail(yougame::update::kMessage);', 'return;'):
            self.assertIn(action, refused)
        self.assertLess(at, blend.index('coord.tick('))

    def test_stdin_notice_is_processed_before_any_platform_window_queue(self):
        link = code(source("port/runtime/hle/yougame_link.cpp"))
        reader = function(link, "handle_app_line")
        self.assertLess(reader.index('g_update_notice.command(line)'), reader.index('#ifdef MELEE_LINUX_X11_EMBED'))
        self.assertIn('return g_update_notice.available() ? update::kMessage : g_status;', function(link, "status_line"))
        # App notices contain no search cancellation, status changes or I/O.
        command = function(code(source("port/runtime/hle/yougame_update.h")), "command")
        for forbidden in ('stop(', 'cancel(', 'fail(', 'host::', 'bridge('):
            self.assertNotIn(forbidden, command)

    def test_version_and_update_reuse_the_existing_status_area(self):
        ui = source("port/runtime/gx/pc_settings.cpp")
        area = ui[ui.index('const bool show_status ='):ui.index('ImGuiStyle launcher_saved_style;')]
        self.assertIn('if (yougame::active() || slippi::online::online_status_visible())', ui)
        self.assertIn('!slippi::online::is_online_match()', area)
        self.assertIn('ImGui::Begin("##yougame_friend_waiting"', area)
        self.assertIn('status == yougame::update::kMessage', area)
        self.assertNotIn('version_line()', area)
        self.assertIn('ImGui::TextUnformatted(status.c_str())', area)
        online = source("port/runtime/hle/slippi_online.cpp")
        self.assertIn('g_matchmaking->GetErrorMessage(), 120)', online)

    def test_online_menu_visibility_is_independent_of_hidden_friends(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        think = function(code(menu), 'mu_slippi_online_menu_think')
        self.assertIn('int online_visible = 1;', think)
        self.assertEqual(think.count('online_visible = 0;'), 2)
        self.assertIn('if (online_visible) friends_menu |= 4;', think)
        online = source("port/runtime/hle/slippi_online.cpp")
        self.assertIn('g_online_menu_visible = (payload[0] & 4) != 0;', online)
        self.assertIn('(payload[0] & 3) != 0, (payload[0] & 3) == 2', online)
        visible = function(code(online), 'online_status_visible')
        self.assertIn('!is_online_match() && (g_online_menu_visible.load() || in_online_menus())', visible)
        self.assertIn('inline bool online_status_visible()', source('port/runtime/host/netplay_state.h'))

    def test_direct_and_doubles_require_grant_and_native_slippi_route(self):
        net = code(source("port/runtime/hle/slippi_net.cpp"))
        start = function(net, 'startMatchmaking')
        self.assertLess(start.index('if (yougame::agent_test::agent_script)'), start.index('const bool slippi_mode'))
        for condition in ('yougame::mode_policy::slippi_menu_mode(',
                          'if ((slippi_mode || (unranked && vanilla)) && user_)',
                          'slippi_user_ = User::GrantedSlippiUser(user_->dir());'):
            self.assertIn(condition, start)
        self.assertIn('slippi_user_->CheckOnlineCompatibility(&error)', start)
        self.assertLess(start.index('prepare_slippi_match('), start.index('openSlippiTicket('))
        self.assertIn('display_provider_ = display::Provider::Slippi;', start)
        self.assertNotIn('search_settings_.connect_code =', start) # Preserve the game's raw Slippi bytes.
        grant = function(net, 'GrantedSlippiUser')
        self.assertIn('if (account::app_sign_in().empty()) return nullptr;', grant)
        self.assertIn('std::make_unique<User>(user_dir, true)', grant)
        self.assertIn('user->IsLoggedIn()', grant)
        self.assertNotIn('direct_code_from_game', start)
        self.assertNotIn('slippi_connect_code', start)
        self.assertNotIn('direct on YouGame', source('port/runtime/hle/slippi_net.cpp'))


if __name__ == '__main__':
    unittest.main()
