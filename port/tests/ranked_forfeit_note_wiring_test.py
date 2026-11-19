"""122.24: a Ranked set the room gives by a leave says so on the CSS the player lands on ("Opponent left: you win",
or "You left ..." on the other side), rated or not, and a final that brings no standing has the rank line read
again. The real result model, read model and reveal reasons compiled; source wiring for the CSS line and the
host's result hook; no real-game flow certification."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


def run_fixture(name, body, *includes):
    with tempfile.TemporaryDirectory(prefix='forfeit-note-' + name + '-') as directory:
        cpp, exe = Path(directory) / 'fixture.cpp', Path(directory) / 'fixture'
        cpp.write_text(body)
        sdk = ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []
        flags = [arg for inc in includes for arg in ('-I', str(ROOT / inc))]
        subprocess.run([*menu.compiler_command(), '-std=c++20', *sdk, *flags, str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


RESULTS = r'''
#include <cstdint>
#include "yougame_ranked.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
using namespace yougame::ranked;
int main() {
  // A test build's set: the room gives it by a leave, unrated. Who left still reaches the game.
  json unrated = {{"t","result"},{"queue","ranked"},{"matchId","set"},{"disposition","forfeit"},
    {"ratings",nullptr},{"ratingDisposition",{{"rated",false},{"reason","The opponent left the match and did not reconnect"}}}};
  for (int won = 0; won < 2; ++won) {
    ResultsPanel p; p.pending("set"); auto e = unrated;
    e["ranking"] = won ? json::array({"me","them"}) : json::array({"them","me"});
    CHECK(p.receive(e, "me") && p.record().status == MU_YG_RESULT_UNRATED);
    CHECK(p.record().reason == (won ? MU_YG_REASON_FORFEIT_WON : MU_YG_REASON_FORFEIT_LOST));
    RankCache cache; CHECK(!cache.result(e, "me"));   // no standing in it: the CSS must read again
  }
  // Unrated without a leave keeps its own reason.
  { ResultsPanel p; p.pending("set"); auto e = unrated; e["disposition"] = "void"; e["ranking"] = json::array({"me","them"});
    e["ratingDisposition"] = {{"rated",false},{"reason","void"}};
    CHECK(p.receive(e, "me") && p.record().reason == MU_YG_REASON_VOID); }
  { ResultsPanel p; p.pending("set"); auto e = unrated; e.erase("disposition"); e["ranking"] = json::array({"me","them"});
    CHECK(p.receive(e, "me") && p.record().reason == MU_YG_REASON_OTHER); }
  // One read per CSS visit; again() allows one more after the final, and the failure backoff still holds.
  RankRead read; read.enter();
  CHECK(read.begin(1000)); RankCache standing; read.answer(standing, json(), 0, 1100);
  CHECK(!read.begin(2000));
  read.again(); CHECK(read.begin(3000));
  read.answer(standing, json(), 0, 3100, true);   // failed: 60 s backoff
  read.again(); CHECK(!read.begin(4000)); CHECK(read.begin(63100));
  std::puts("forfeit note: unrated leave keeps who left; the rank line reads again after the final");
}
'''

REVEAL = r'''
#define MU_RANKED_REVEAL_TEST
#include "../../sourceport/game/shim/mu_ranked_reveal.c"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
int main() {
  CHECK(!std::strcmp(reveal_reason(MU_YG_REASON_FORFEIT_WON), "Opponent left - set not rated"));
  CHECK(!std::strcmp(reveal_reason(MU_YG_REASON_FORFEIT_LOST), "You left - set not rated"));
  // The unrated forfeit keeps its reason through the panel model (numbers and ranks still cleared).
  MuReveal s{}; MuYgResult r{}; r.status = MU_YG_RESULT_UNRATED; r.reason = MU_YG_REASON_FORFEIT_WON; r.after = 5;
  reveal_accept(&s, &r);
  CHECK(s.active && s.result.reason == MU_YG_REASON_FORFEIT_WON && s.result.after == 0);
  std::puts("forfeit note: the reveal names an unrated leave");
}
'''


class RankedForfeitNoteTest(unittest.TestCase):
    def test_result_and_read_models(self):
        run_fixture('results', RESULTS, 'port/runtime/hle', 'port/third_party')

    def test_reveal_reasons(self):
        with tempfile.TemporaryDirectory(prefix='forfeit-reveal-') as directory:
            cpp, exe = Path(directory) / 'fixture.cpp', Path(directory) / 'fixture'
            # The include is relative to port/tests, as yougame_ranked_reveal_test.cpp's is.
            cpp.write_text(REVEAL.replace('../../sourceport', str(ROOT / 'sourceport')))
            sdk = ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []
            subprocess.run([*menu.compiler_command(), '-std=c++17', *sdk, '-I', str(ROOT / 'port/runtime/abi'),
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_host_reads_the_standing_again_after_a_final_without_one(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        at = link.index('if (g_rank.result(m, r.set.local_participant())) host::log("yougame: ranked standing')
        self.assertIn('else g_rank_read.again();', link[at:at + 400])
        ranked = source('port/runtime/hle/yougame_ranked.h')
        self.assertIn('void again() { asked = false; }', ranked)
        self.assertEqual(ranked.count('forfeit_reason(event, participant)'), 2)

    def test_css_rank_panel_shows_the_leave(self):
        rank = source('sourceport/game/shim/mu_ranked_rank.c')
        self.assertIn('ranks.result.reason == MU_YG_REASON_FORFEIT_WON', rank)
        self.assertIn('ranks.result.reason == MU_YG_REASON_FORFEIT_LOST', rank)
        frame = rank[rank.index('void mu_ranked_rank_frame(void)'):]
        note = frame.index('if (panel.left && ms->connection_state == MU_SLP_MM_IDLE && !mu_ranked_practice())')
        self.assertLess(note, frame.index('set->cur_game > 1 && set->max_games'))
        for words in ('"Opponent left: you win"', '"You left: counted as a loss"', '"You left the set"'):
            self.assertIn(words, frame)
        self.assertIn('score_line(score);', frame)
        self.assertNotIn('line(SET_SCORE, score);', frame)
        # Game C only: no strstr/strchr added for the note.
        helper = rank[rank.index('static void score_line'):rank.index('static void free_panel')]
        self.assertNotIn('strstr', helper)
        self.assertNotIn('strchr', helper)

    def test_wire_comment_allows_unrated_forfeit(self):
        self.assertIn('RECORDED or UNRATED: the room gave the set by a leave', source('port/runtime/abi/mu_ranked_wire.h'))


if __name__ == '__main__':
    unittest.main()
