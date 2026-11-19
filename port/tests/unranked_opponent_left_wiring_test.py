"""122.26: an Unranked session the opponent ends (they leave mid-game or between games) says "Opponent left" on the
character select the player lands on, until the next search, as Ranked's CSS has since 122.24. The real policy
compiled; source wiring for the host's flag, its query and the CSS line; no real-game flow certification."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


POLICY = r'''
#include "yougame_unranked.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
using yougame::unranked::opponent_left;
int main() {
  // session, mode (1 Unranked), friend room, was connected, own room dropped
  CHECK(opponent_left(true, 1, false, true, false));
  CHECK(!opponent_left(false, 1, false, true, false));   // standalone Slippi: no YouGame session
  CHECK(!opponent_left(true, 0, false, true, false));    // Ranked has its own note
  CHECK(!opponent_left(true, 2, false, true, false));    // Direct / Friends
  CHECK(!opponent_left(true, 1, true, true, false));     // a friend room
  CHECK(!opponent_left(true, 1, false, false, false));   // never connected (Found: then gone): searches again
  CHECK(!opponent_left(true, 1, false, true, true));     // our own connection died with the link
  std::puts("unranked opponent left: only a connected YouGame Unranked session whose own room stayed up");
}
'''


class UnrankedOpponentLeftTest(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory(prefix='unranked-left-') as directory:
            cpp, exe = Path(directory) / 'fixture.cpp', Path(directory) / 'fixture'
            cpp.write_text(POLICY)
            sdk = ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []
            subprocess.run([*menu.compiler_command(), '-std=c++17', *sdk, '-I', str(ROOT / 'port/runtime/hle'),
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_host_sets_the_flag_only_where_the_opponents_link_dropped(self):
        online = source('port/runtime/hle/slippi_online.cpp')
        self.assertIn('bool g_unranked_opponent_left = false;', online)
        # Two places set it: the match-state poll's "connected, but the link is gone" branch, and (122.28) the
        # first in-game input poll that finds the opponent gone. The second survives the game's own cleanup.
        self.assertEqual(online.count('g_unranked_opponent_left = true;'), 2)
        state = online[online.index('void prepare_online_match_state(std::vector<uint8_t>& q) {'):]
        at = state.index('g_unranked_opponent_left = true;')
        branch = state[state.rindex('} else {', 0, at):at]
        # Read before the cleanup (which forgets the session, its room and g_play_session_active), set after it.
        decide = branch.index('yougame::unranked::opponent_left(')
        self.assertLess(decide, branch.index('cleanup_connection();'))
        for arg in ('yougame::session()', '(int)g_last_search.mode', '!g_last_search.friend_room.empty()',
                    'g_play_session_active', 'yougame::own_room_dropped()'):
            self.assertIn(arg, branch[decide:branch.index('cleanup_connection();')])
        # Every cleanup resets it to the in-game leave (122.28), so a lobby-side note never outlives its cleanup
        # but a game the opponent ended keeps it; Start (a new search) clears both.
        cleanup = online[online.index('void cleanup_connection(yougame::Leave why'):]
        self.assertIn('g_unranked_opponent_left = g_unranked_left_in_game;', cleanup[:cleanup.index('finish_summary(')])
        find = online[online.index('    case CMD_FIND_OPPONENT:'):]
        self.assertIn('g_unranked_opponent_left = g_unranked_left_in_game = false;',
                      find[:find.index('start_find_match(payload)')])

    def test_in_game_leave_sets_it_before_the_disconnect_reply(self):
        online = source('port/runtime/hle/slippi_online.cpp')
        inputs = online[online.index('  if (is_disconnected()) {\n    // YouGame Unranked'):]
        inputs = inputs[:inputs.index('q.push_back(3);')]
        self.assertIn('!is_leaving', inputs)   # this player's own quit never shows it
        self.assertIn('g_unranked_left_in_game = g_unranked_opponent_left = true;', inputs)
        self.assertLess(inputs.index('g_unranked_left_in_game = g_unranked_opponent_left = true;'),
                        inputs.index('finish_summary("disconnected");'))
        for arg in ('yougame::session()', '(int)g_last_search.mode', '!g_last_search.friend_room.empty()',
                    'g_play_session_active', 'yougame::own_room_dropped()'):
            self.assertIn(arg, inputs)
        # The game reads it through the optional data subcommand.
        data = online[online.index('    case CMD_YOUGAME_DATA:'):]
        data = data[:data.index('default: return false; // optional subcommand unknown to this host')]
        self.assertIn('case MU_YG_SUB_UNRANKED_NOTE:\n          q.assign(1, g_unranked_opponent_left ? 1 : 0);', data)

    def test_own_drop_reads_the_room_socket(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        body = link[link.index('bool own_room_dropped() {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('std::lock_guard<std::mutex> lock(g_mutex);', body)
        self.assertIn('own_dropped() || g_room->socket->recoverable()', body)
        # An open connection reads 1006 too: only a closed one counts as our own drop.
        self.assertIn('g_room->socket->connection_closed() &&', body)
        sock = link[link.index('bool connection_closed() const {'):]
        sock = sock[:sock.index('\n  }\n')]
        self.assertIn('if (broker_) return broker_->closed();', sock)
        self.assertIn('return closed_.load();', sock)
        self.assertIn('bool own_room_dropped();', source('port/runtime/hle/yougame_link.h'))

    def test_wire_subcommand(self):
        wire = source('port/runtime/abi/mu_ranked_wire.h')
        self.assertIn('#define MU_YG_SUB_UNRANKED_NOTE 5u', wire)
        for taken in ('MU_YG_SUB_RANK 0u', 'MU_YG_SUB_POLICY 1u', 'MU_YG_SUB_PRACTICE 2u',
                      'MU_YG_SUB_PRACTICE_STATUS 3u', 'MU_YG_SUB_PRACTICE_ALLOWED 4u'):
            self.assertIn('#define ' + taken, wire)

    def test_css_says_opponent_left_in_unranked_idle_only(self):
        css = source('sourceport/game/shim/mu_slippi_css.c')
        helper = css[css.index('static int unranked_opponent_left(void)'):css.index('static void set_text(int idx')]
        self.assertIn('MuYgU8 sub = MU_YG_SUB_UNRANKED_NOTE', helper)
        self.assertIn('mu_slippi_cmd(MU_YG_COMMAND, &sub, 1, &left, 1, &got) == 0 && got == 1 && left == 1', helper)
        think = css[css.index('static void text_think(HSD_GObj* gobj)'):]
        press = think[think.index('/* Press / hold Z'):think.index('set_text(ST_PRESS_Z, prompt);')]
        note = press.index('conn == MU_SLP_MM_IDLE && st->mode == MU_SLP_MODE_UNRANKED && unranked_opponent_left()')
        # After connected / error / searching: a search in progress says "Press Z to cancel" instead.
        self.assertLess(press.index('conn > MU_SLP_MM_IDLE'), note)
        self.assertIn('strcpy(prompt, "Opponent left");', press[note:])
        # Game C only: no strstr/strchr added for the note.
        for code in (helper, press):
            self.assertNotIn('strstr', code)
            self.assertNotIn('strchr', code)


if __name__ == '__main__':
    unittest.main()
