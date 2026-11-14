"""Friends: a full room is a final answer with its own line, and B out of the friend room's CSS lands on Friends.

Host: the invite route refuses a full room (409, code "room_full"); the toast shows the site's words.
Guest: the app refuses the room upgrade ("the rooms server answered 409: Room is full"); the guest sees
"<host>'s room is full.", the search is never retried, and the game goes back to the Friends menu.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HLE = 'port/runtime/hle/'


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def body(source, name):
    at = source.index(name)
    at = source.index('{\n', at)   # the body, past any "= {}" default argument
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end]


def in_order(test, text, parts):
    at = -1
    for part in parts:
        found = text.find(part, at + 1)
        test.assertGreater(found, at, part)
        at = found


class FriendsRoomFull(unittest.TestCase):
    def test_host_invite_refusal_keeps_the_sites_words(self):
        link = read(HLE + 'yougame_link.cpp')
        self.assertIn(': friends::kRequestFailed;', body(link, 'json bridge(const json& body'))
        picker = body(link, 'std::string pick_friend_room(')
        handler = picker[picker.index('} catch (const std::exception& e) {'):]
        # The refusal code comes from this thread's last bridge answer; a cancel is never "full".
        in_order(self, handler, ['friends::invite_room_full(g_bridge_refusal, e.what())',
                                 'log_friend_version();', 'host::log("friends: cancelled");',
                                 'host::log("friends: invite failed: room full");',
                                 'throw friends::RoomFull(full);',
                                 'picker failed (%s room)', 'throw;'])
        self.assertNotIn('%s", e.what()', handler)   # never log the site's text unbounded
        tick = body(read(HLE + 'native_practice.cpp'), 'void tick()')
        # Both Friends menus that show an invite error today: the held-room CSS (Z) and the main menu (A).
        for generic in ['"Couldn\'t open your friends list. Press Z to try again."',
                        '"Couldn\'t open your friends list. Press A to try again."']:
            at = tick.index(generic)
            full = tick.rindex('catch (const yougame::friends::RoomFull& e) {', 0, at)
            generic_catch = tick.index('catch (const std::exception& e) {', full)
            self.assertLess(generic_catch, at)
            self.assertIn('yougame::friends::events.notice(e.what(),', tick[full:generic_catch])

    def test_guest_upgrade_refusal_is_final_and_names_the_host(self):
        transport = read(HLE + 'yougame_transport.h')
        self.assertIn('"room is full"', body(transport, 'inline bool rooms_full('))
        stream = body(transport, 'class Stream {')
        self.assertIn('busy_ = false; full_ = false;', body(stream, 'bool open(const std::string& ticket'))
        close = body(stream, 'void record_close(')
        for case in ['(code_ == 1006 && rooms_full(p))', '(code_ == 1008 && reason == "room full")',
                     '(code_ == 4004 && reason == "rejoin-refused:room-full")']:
            self.assertIn(case, close)
        error = stream[stream.index('void record_error(const json& p) {'):]
        error = error[:error.index('\n  }\n') + 4]
        self.assertIn('if (str(p, "code") == "refused" && rooms_full(p)) full_ = true;', error)
        self.assertNotIn('return', error)  # the full refusal is recorded whatever else the error names
        for loop in ['bool next(json& m, int ms)', 'bool wait_open(uint64_t until']:
            self.assertIn('if (event == "error") record_error(p);', body(stream, loop))

        link = read(HLE + 'yougame_link.cpp')
        # Every other 1006 drop keeps its recovery: only the friend paths read the full mark.
        self.assertIn('bool recoverable() const { return broker_ && broker_->close_code() == 1006; }', link)
        failed = body(link, 'Match fail_room_full(')
        in_order(self, failed, ['friends::room_full_line(friends::events.inviter(room_code))',
                                'if (search_stopped()) return m;', 'set_status("offline");',
                                'friends::events.room_full(m.error, now_ms());'])
        self.assertNotIn('m.retry = true', failed)
        search = body(link, 'Match find_match(')
        first = search.index('if (!adopted && !room->socket->open_session(url, pass, room_code, &error, 10000, cancelled)) {')
        self.assertLess(search.index('return fail_room_full(room_code);', first),
                        search.index('return fail(friends::events.refusal_text(room_code, error, false), !direct);', first))
        join = search[search.index('if (t == "error") return fail(m.value("error", m.value("message",'):]
        self.assertIn('friends_mode && t == "command-error" && m.value("action", "") == "join"', join[:400])
        self.assertIn('friends::says_room_full(ranked::str(m, "error"))) return fail_room_full(room_code);', join[:400])
        closed = search[search.index('} else if (room->socket->closed()) {'):]
        closed = closed[:closed.index('continue;')]
        in_order(self, closed, ['const auto seats_full = [&] { return friends_mode && room->socket->room_full(); };',
                                'if (seats_full()) return fail_room_full(room_code);',
                                'if (!room->socket->recoverable())',
                                'if (seats_full()) break;',
                                'if (!reopened && !cancelled() && seats_full()) return fail_room_full(room_code);',
                                'if (!reopened) return fail("Lost the connection to the YouGame room", !direct);'])
        # The console room never spends its 60 s held-seat rejoin on a seat that is gone.
        self.assertIn('room->rejoinable() && !room->room_full()) {', link)

        tick = body(read(HLE + 'native_practice.cpp'), 'void tick()')
        back = tick[tick.index('events.take_full_return(current_major() == kOnlineMajor,'):]
        back = back[:back.index('if (yougame::console::console_active()) return;')]
        in_order(self, back, ['slippi::online::native_cleanup_match();',
                              'g_bridge(OP_FRIENDS_RETURN,a,1);', 'request_major(1);'])

    def test_b_out_of_the_friend_rooms_css_lands_on_friends(self):
        online = read(HLE + 'slippi_online.cpp')
        start = body(online, 'void start_find_match(')
        self.assertTrue(start.lstrip('{').lstrip().startswith('if (!friend_room.empty()) g_friends_css = true;'))
        menu = online[online.index('    case CMD_YOUGAME_FRIENDS_MENU:'):online.index('    case CMD_YOUGAME_DATA:')]
        self.assertLess(menu.index('if (payload[0] == 14)'), menu.index('g_online_menu_visible = (payload[0] & 4) != 0;'))
        self.assertIn('q.assign(1, g_friends_css.exchange(false) ? 1 : 0);', menu)
        shim = read('sourceport/game/shim/mu_slippi_menu.c')
        enter = body(shim, 'int mu_slippi_menu_enter(')
        # Asked before any early return, so every entry consumes the one-shot answer.
        self.assertLess(enter.index('u8 friends_query = 14'), enter.index('if (!mu_slippi_menus_enabled())'))
        back = enter[enter.index('if (previous_mode == GM_HANYU_CSS) {'):]
        in_order(self, back, ['if (ranked_supported && from_friends_online) {', 'slippi_submenu = 0;',
                              '*hovered = (u8) mu_online_menu_row(1, 0, MU_SLP_OPT_FRIENDS);',
                              '} else if (ranked_supported) {'])
        # Leaving the friend room's CSS still ends the room: nothing here keeps it.
        self.assertNotIn('friends_lobby', back[:back.index('} else if (ranked_supported) {')])

    def test_header_cases_run(self):
        compiler = shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('no C++ compiler')
        with tempfile.TemporaryDirectory(prefix='friends-full-') as directory:
            for name in ['yougame_friends_test', 'yougame_transport_test']:
                exe = Path(directory) / name
                subprocess.run([compiler, '-std=c++17', '-I', str(ROOT / HLE), '-I', str(ROOT / 'port/third_party'),
                                str(ROOT / 'port/tests' / (name + '.cpp')), '-o', str(exe), '-pthread'], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
