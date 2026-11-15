"""Update-only port from 589c907: offline source checks, no service requests."""
from pathlib import Path
import unittest
import re
import tempfile
import subprocess
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
    def test_ready_is_first_and_consumed_before_all_other_commands(self):
        reader = literal_function(source('port/runtime/hle/yougame_link.cpp'), 'handle_app_line')
        self.assertTrue(reader.lstrip().startswith('if (g_update_notice.command(line)) {'))
        branch = reader[:reader.index('  if (line.rfind("yg-pads ')]
        self.assertIn('return;', branch)
        self.assertIn('update: ready %s (waiting for a safe point)', branch)

    def test_production_stdout_writer_accepts_once_and_requests_clean_exit(self):
        from online_menu_test import compiler_command
        link = source('port/runtime/hle/yougame_link.cpp')
        reader = literal_function(link, 'handle_app_line')
        reader = reader[:reader.index('  if (line.rfind("yg-pads ')] + '\n ++forwarded;\n'
        harness = r'''
#include "yougame_update.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace yougame;
using HANDLE = int; using DWORD = unsigned;
constexpr int STD_OUTPUT_HANDLE = 1, INVALID_HANDLE_VALUE = -1;
static std::vector<std::string> writes, logs;
static int forwarded, exits;
static uint64_t clock_ms;
static uint64_t now_ms() { return clock_ms; }
struct { std::mutex write_mutex; } g_io;
static update::Notice g_update_notice;
static HANDLE GetStdHandle(int) { return 1; }
static void WriteFile(HANDLE, const char* p, DWORD n, DWORD* wrote, void*) {
  writes.emplace_back(p,n); *wrote=n;
}
namespace host {
void log(const char* fmt, ...) { logs.emplace_back(fmt); }
void set_replay_viewing(bool) {}
void request_user_exit() { ++exits; }
}
'''
        harness += 'void write_line(const std::string& line) {' + literal_function(link, 'write_line') + '}\n'
        harness += 'void handle_app_line(const std::string& line) {' + reader + '}\n'
        harness += 'void update_notice_presented() {' + literal_function(link, 'update_notice_presented') + '}\n'
        harness += r'''
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  const std::string id="01234567-89ab-cdef-0123-456789abcdef";
  handle_app_line("embed update_ready bad");
  handle_app_line("embed update_ready");
  CHECK(!forwarded && g_update_notice.pending().empty() && logs.empty());
  handle_app_line("embed update_ready " + id);
  CHECK(!forwarded && g_update_notice.pending()==id && logs.size()==1);
  handle_app_line("embed volume 1"); CHECK(forwarded==1);
  CHECK(g_update_notice.begin({true,false,false,false,false}));
  clock_ms=300; update_notice_presented(); CHECK(writes.empty() && !exits);
  clock_ms=1299; update_notice_presented(); CHECK(writes.empty() && !exits);
  clock_ms=1300; update_notice_presented();
  CHECK(writes==std::vector<std::string>{"yougame: update accept " + id + "\n"});
  CHECK(exits==1);
  clock_ms=2000; update_notice_presented(); update_notice_presented();
  CHECK(exits==1 && writes.size()==1 && logs.size()==2);
  std::puts("production ready routing, stdout newline and once-only clean exit verified");
}
'''
        with tempfile.TemporaryDirectory(prefix='update-stdout-') as directory:
            cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
            cpp.write_text(harness)
            subprocess.run(compiler_command() + ['-std=c++17', '-pthread', '-I'+str(ROOT/'port/runtime/hle'),
                '-I'+str(ROOT/'port/third_party'), str(cpp), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_safe_point_uses_session_search_room_retirement_and_friends(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        safe = literal_function(link, 'update_safe_point')
        for signal in ('online_busy || g_update_searches.load() != 0',
                       'bool(g_room) || g_slippi_match || retiring', 'g_room && g_room->friends',
                       'g_retired.wait_for', 'g_update_notice.begin(state)'):
            self.assertIn(signal, safe)
        self.assertIn('++g_update_searches', literal_function(link, 'find_match'))
        self.assertIn('--g_update_searches', literal_function(link, 'find_match'))
        self.assertIn('update: safe point, handing back to YouGame', safe)
        # Opening Online asks the app for an update check at once, once per opening (11-10).
        self.assertIn('if (online_screen && !was_online_screen && g_stdio) write_line(update::kCheckLine);', safe)
        self.assertIn('kCheckLine = "yougame: update check"', source('port/runtime/hle/yougame_update.h'))

    def test_every_room_key_carries_the_online_protocol(self):
        # Builds 88 and 92 shared "mu-0.9.110" and broke at an invite (11-11): every key the game sends is
        # "mu-<version>-p<N>", N bumped by hand under the rule beside kOnlineProtocol.
        compat = source('port/runtime/hle/yougame_compatibility.h')
        self.assertIn('inline constexpr int kOnlineProtocol = ', compat)
        self.assertIn('BUMP RULE', compat)
        link = source('port/runtime/hle/yougame_link.cpp')
        self.assertIn('const std::string v = online_version(yougame::build_version());', link)
        self.assertIn('return lobby::compatibility(online_version(yougame::build_version()), [&]', link)
        self.assertIn('compatibility_key(online_version(MELEE_PORT_VERSION),false,"",false) + "-console5"', link)
        self.assertNotRegex(link, r'(compatibility_key|lobby::compatibility|squad::compatibility)\((yougame::build_version\(\)|MELEE_PORT_VERSION|"dev")')

    def test_scene_loop_is_held_and_renderer_repaints_without_subframes(self):
        host = source('port/app/source_host.cpp')
        wait = literal_function(host, 'h_vi_wait_retrace')
        self.assertIn('if (g_resim)', wait)
        self.assertIn('slippi::online::session_mode() >= 0', wait)
        self.assertIn('while (yougame::update_safe_point(', wait)
        self.assertIn('!g_update_ranked_busy || g_update_ranked_busy() != 0', wait)
        self.assertIn('slippi::online::online_status_visible()))', wait)  # " online" accept suffix
        self.assertIn('game_proc("mu_update_ranked_busy_v1")', host)
        repaint = source('port/runtime/gx/threaded_backend.cpp')
        self.assertIn('(replay_control::repaint_wanted() || yougame::update_handoff_visible())', repaint)
        ui = source('port/runtime/gx/pc_settings.cpp')
        notice = literal_function(ui, 'draw_update_handoff_notice')
        for text in ('bounds.left + bounds.right', 'bounds.top + bounds.bottom', 'ImVec2(0.5f, 0.5f)',
                     'ImGuiWindowFlags_NoInputs', 'ImGui::TextUnformatted("Updating...")',
                     'yougame::update_notice_presented();'):
            self.assertIn(text, notice)
        self.assertIn('draw_update_handoff_notice();\n  ImGui::Render();', ui)

    def test_ranked_guard_waits_for_real_result_reveal(self):
        game = source('sourceport/game/shim/mu_ranked_setup_online.c')
        self.assertIn('update_set_active = 1;', literal_function(game, 'mu_ranked_setup_begin'))
        finish = literal_function(game, 'mu_ranked_setup_finish_game')
        self.assertIn('result == MU_SETUP_SET_OVER || result == MU_SETUP_SET_ABORTED', finish)
        self.assertNotIn('mu_ranked_update_reset', finish)
        reveal = literal_function(source('sourceport/game/shim/mu_ranked_reveal.c'), 'mu_ranked_reveal_frame')
        self.assertIn('reveal.panel && previous >= REVEAL_GLOW_END', reveal)
        self.assertLess(reveal.index('reveal_draw();'), reveal.index('mu_ranked_update_result_shown();'))
        self.assertIn('reveal.panel && !mu_ranked_practice() && reveal_shown_line(s, line)', reveal)
        self.assertIn('mu_ranked_update_reset();', literal_function(
            source('sourceport/game/shim/mu_slippi_menu.c'), 'online_on_unload'))

    def test_resume_reuses_existing_boot_and_never_selects_a_mode(self):
        main = source('port/app/main.cpp')
        resume = main[main.index('    if (a == "--resume")'):main.index('    if (a == "--vanilla-only") continue;')]
        self.assertIn('yougame::update::resume(value)', resume)
        self.assertIn('source_port::set_resume_online();', resume)
        self.assertEqual(resume.count('resume: unknown value ignored'), 1)
        for forbidden in ('set_online_test', 'native_start_match', 'set_match('):
            self.assertNotIn(forbidden, resume)
        host = source('port/app/source_host.cpp')
        self.assertIn('g_slippi_menus ? "resume: online', host)
        self.assertIn('Online boot menu unavailable', host)
        boot = literal_function(source('sourceport/game/shim/mu_slippi_menu.c'), 'mu_slippi_boot_mode')
        self.assertIn('boot_oneshot = 1;', boot)
        self.assertIn('return GM_MENU;', boot)

    def test_yougame_start_boots_main_menu_unless_online_asked(self):
        # Build 103: a YouGame start for a local match opens the main menu, not 1-P > Online Play.
        # --resume online (the app's update relaunch) keeps the boot to Online.
        host = source('port/app/source_host.cpp')
        self.assertIn('if (g_resume_online) slippi::online::request_boot_online();', host)
        online = source('port/runtime/hle/slippi_online.cpp')
        query = online[online.index('case CMD_YOUGAME_RANKED:'):]
        query = query[:query.index('if (yougame::active()) {')]
        self.assertIn('if (yougame::active() && !g_boot_online) q[0] |= 32;', query)
        shim = source('sourceport/game/shim/mu_slippi_menu.c')
        for name in ('mu_slippi_ranked_line', 'mu_slippi_ranked_lines'):
            self.assertIn('boot_main_menu = (r[0] & 32) != 0;', literal_function(shim, name))
        enter = literal_function(shim, 'mu_slippi_menu_enter')
        boot = enter[enter.index('if (previous_mode == GM_BOOT && boot_oneshot) {'):]
        # Cleared before it can return, so a later menu entry never reopens Online.
        self.assertLess(boot.index('boot_oneshot = 0;'), boot.index('if (boot_main_menu && !mu_online_p2p_menus())'))
        self.assertLess(boot.index('mu_slippi_fetch_app_state();'), boot.index('if (boot_main_menu'))

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
        # The room's, the queue's (a Teams search), and the Doubles home's own queue socket (G2).
        self.assertEqual(find.count('if (update::required(refused))'), 3)
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
                     'ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str())'):
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
        # request_ticket and team_ticket, then the 4-player Friends lobby's lobby_hold and lobby_ticket,
        # and the Doubles home's doubles_lobby and doubles_hold, whose key (lobby_room_key) is built from
        # the same tagged compatibility_key.
        self.assertEqual(friends.count('{"compatibility", compatibility}'), 6)
        lobby = function(code(raw), "lobby_room_key")
        self.assertIn('compat_tag_with_test_timer(compat_tag', lobby)
        self.assertIn('friends::lobby_hold(lobby_key)', find)
        self.assertIn('friends::lobby_ticket(lobby_key, lobby_room)', find)
        self.assertIn('friends::doubles_hold(lobby_key)', find)
        self.assertIn('friends::doubles_home_ticket(lobby_key, lobby_room)', find)
        self.assertIn('friends::doubles_queue_ticket(lobby_key, room_code, queue_generation, lobby_session->owns_local() ? 1 : party.humans)', find)
        # The match room's admission renewals (review G2 #3): a match-admission ticket on the same key, never
        # the queue's ticket (party_room, partySearch, party).
        self.assertIn('if (doubles_room) request = friends::doubles_match_ticket(lobby_key);', find)
        self.assertNotIn('doubles_match.request', raw)
        # The lobby searches in one mode, never retries into a new room, and closes its links uncancelled.
        self.assertIn('search.friends_lobby && (search.mode != lobby::kSearchMode', find)
        self.assertIn('LobbySearch', find)
        self.assertIn('if (g_lobby_search) m.retry = false;', function(code(raw), "fail"))
        self.assertIn('early.request = close_app;', find)
        self.assertIn('early.links.hand_over<update::Required>(', find)
        self.assertIn('early.links.open<update::Required>(', find)
        self.assertNotIn('early.links.for_peers(', find)
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
        self.assertIn('ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str())', area)
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
        self.assertIn('slippi_user_->CheckSessionCompatibility(&error)', start)
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
