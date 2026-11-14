"""Production confirmed-digest buffer, encoder and observation fixtures.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from squad_wiring_test import ROOT, CREDIT, function, read, without_squad


class SquadConfirmedDigest(unittest.TestCase):
    def run_cpp(self, source, extra=()):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            cpp, exe = Path(tmp) / 'fixture.cpp', Path(tmp) / 'fixture'
            cpp.write_text(source)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                            '-I', str(ROOT / 'port/runtime/host'), '-I', str(ROOT / 'port/runtime/abi'),
                            str(cpp), '-o', str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe), str(Path(tmp) / 'digest.csv'), *extra], check=True,
                           capture_output=True, text=True)

    def test_production_buffer_resimulation_frontier_order_tail_and_no_duplicates(self):
        self.run_cpp(r'''
#include "squad_digest.h"
#include <cassert>
#include <vector>
int main(int, char**) {
  host::squad_digest::Pending pending;
  std::vector<host::squad_digest::Row> written;
  auto emit = [&](const auto& row) { written.push_back(row); };
  MuStatePod state{};
  state.match_frame = 1; state.rng = 0xBAD;
  pending.sample(100, 5, state); // predict F (body and VS frame have different origins)
  pending.confirm(99, emit); assert(written.empty());
  state.rng = 0xC0FFEE;
  pending.sample(100, 6, state); // resim F overwrites predicted state
  state.match_frame = 2; state.rng = 2;
  pending.sample(101, 6, state); // unconfirmed tail
  pending.confirm(100, emit);
  assert(written.size() == 1 && written[0].state.rng == 0xC0FFEE);
  assert(written[0].state.match_frame == 1 && written[0].retrace == 6);
  pending.confirm(100, emit); // repeated frontier emits nothing twice
  state.match_frame = 1; pending.sample(100, 8, state);
  pending.confirm(100, emit); assert(written.size() == 1);
  pending.discard_tail(); pending.confirm(999, emit); assert(written.size() == 1);
  host::squad_digest::Pending catchup;
  state.match_frame = 1; state.rng = 1; catchup.sample(100, 10, state);
  state.match_frame = 2; state.rng = 2; catchup.sample(101, 11, state);
  state.match_frame = 3; state.rng = 3; catchup.sample(102, 12, state);
  // A raw frontier of 102 is clamped to completed body 100 while rollback catches up.
  state.match_frame = 1; state.rng = 11; catchup.sample(100, 13, state);
  catchup.confirm(std::min(100, 102), emit);
  assert(written.size() == 2 && written.back().state.rng == 11);
  state.match_frame = 2; state.rng = 22; catchup.sample(101, 13, state);
  catchup.confirm(std::min(101, 102), emit);
  state.match_frame = 3; state.rng = 33; catchup.sample(102, 13, state);
  catchup.confirm(102, emit);
  assert(written.size() == 4 && written[2].state.rng == 22 && written[3].state.rng == 33);
}
''')

    def test_late_resim_is_keyed_by_body_not_a_held_match_frame(self):
        self.run_cpp(r'''
#include "squad_digest.h"
#include <cassert>
#include <vector>
using namespace host::squad_digest;
int main(int, char**) {
  Pending pending;
  std::vector<Row> written;
  auto emit = [&](const auto& row) { written.push_back(row); };
  LateRow late{};
  MuStatePod state{};
  // Startup: the VS counter sits at 0 for several bodies before any write.
  for (int32_t body = 90; body < 95; ++body) {
    state.rng = uint32_t(body);
    assert(pending.sample(body, 1, state, &late) == Late::none);
  }
  pending.confirm(94, emit); assert(written.empty()); // match_frame 0 is never a row
  assert(pending.sample(95, 1, state, &late) == Late::none);
  state.match_frame = 1; state.rng = 7;
  assert(pending.sample(100, 5, state) == Late::none);
  pending.confirm(100, emit); assert(written.size() == 1);
  // GO/GAME!/pause: new, larger bodies on the held counter with a different state: never late.
  state.rng = 8; state.player[0].anim_frame = 3;
  assert(pending.sample(101, 6, state, &late) == Late::none && late.body == 0);
  assert(pending.sample(102, 6, state, &late) == Late::none);
  pending.confirm(102, emit); assert(written.size() == 1); // no re-emit of match_frame 1
  assert(pending.held() == 7); // 91..95 on frame 0, 101..102 on frame 1 (100 moved it)
  // A resim of a final body with the same state (a selftest rollback): counted, no row.
  state.rng = 7; state.player[0].anim_frame = 0;
  assert(pending.sample(100, 7, state, &late) == Late::same && late.body == 0);
  state.menu_kind = 3; assert(pending.sample(100, 7, state) == Late::same); // not a digest field
  // A resim of a final body that was never written (held counter): compared with its own state.
  state.rng = 8; state.player[0].anim_frame = 3; assert(pending.sample(101, 7, state) == Late::same);
  state.player[2].percent = 9;
  assert(pending.sample(101, 7, state, &late) == Late::different);
  assert(late.body == 101 && late.match_frame == 1 && late.confirmed_match_frame == 1 &&
         late.frontier_at_confirm == 102);
  // A resim of a written body with a different state.
  state.rng = 99; assert(pending.sample(100, 7, state, &late) == Late::different && late.body == 100);
  assert(written.size() == 1); // never re-emitted
  // Only the last 128 final bodies are kept.
  for (int32_t body = 103; body <= 300; ++body) {
    state.match_frame = uint32_t(body - 101); pending.sample(body, 8, state);
  }
  pending.confirm(300, emit);
  assert(pending.sample(150, 9, state) == Late::unknown);
  state.match_frame = 199; assert(pending.sample(300, 9, state) == Late::same);
}
''')

    def test_extracted_production_observer_writes_resim_state_and_drops_end_tail(self):
        host = read(ROOT / 'port/runtime/host/host.cpp')
        observer = function(host, 'squad_digest_observe')
        digest = function(host, 'digest_state')
        source = r'''
#include "squad_digest.h"
#include <cassert>
#include <string>
#include <fstream>
namespace hle { void dvd_settle() {} }
namespace host {
struct { std::string state_digest; } options;
static FILE* g_state_digest;
static FILE* g_squad_confirmed = nullptr;
static FILE* g_squad_presented_frontier = nullptr;
static FILE* g_squad_late = nullptr;
static squad_digest::Pending g_squad_pending;
static int32_t g_squad_body = -1, g_squad_frontier = -1;
static uint32_t g_squad_match_frame = 0, g_squad_late_same = 0, g_squad_late_unknown = 0, g_retraces = 5;
static std::string logged;
static void log(const char* format, ...) { logged += format; logged += "\n"; }
static MuStatePod fake;
static void snapshot(MuStatePod*) {}
static auto native_state_snapshot = snapshot;
static void read_digest_state(MuStatePod& state) { state = fake; }
static void die(const char*) { assert(false); }
static uint32_t digest_ram_interval() { return 0; }
static uint64_t ram_hash() { return 0; }
''' + observer + digest + r'''
}
int main(int, char** argv) {
  using namespace host;
  options.state_digest = argv[1];
  g_state_digest = std::tmpfile(); assert(g_state_digest);
  fake.match_frame = 1; fake.rng = 0xBAD;
  squad_digest_observe(100, 99, false);
  digest_state(); // predicted presentation
  fake.rng = 0xC0FFEE;
  for (unsigned i=0; i<6; ++i) {
    uint32_t* words = &fake.player[i].present;
    for (unsigned k=0; k<12; ++k) words[k] = 0x12340000 + 12*i + k;
  }
  ++g_retraces;
  squad_digest_observe(100, 100, false);
  digest_state(); // already confirmed presentation
  fake.match_frame = 2; fake.rng = 2;
  squad_digest_observe(101, 100, false);
  squad_digest_observe(101, 100, true); // match ends, tail not confirmed
  ++g_retraces; digest_state(); // a later menu retrace can retain the VS counter: no tag row
  squad_digest_observe(101, 999, true); // discarded tail cannot leak out
  std::ifstream file(options.state_digest + ".confirmed.csv");
  std::string header, row, tail;
  assert(std::getline(file, header) && std::getline(file, row) && !std::getline(file, tail));
  assert(header.rfind("frame,rng,scene,", 0) == 0);
  assert(row.rfind("6,00C0FFEE,00000000,", 0) == 0);
  assert(row.substr(row.size() - 9) == ",00000001");
  // Exact field order and encoding must equal the existing presented writer.
  std::rewind(g_state_digest);
  char buffer[2048];
  assert(std::fgets(buffer, sizeof buffer, g_state_digest)); // prediction
  assert(std::fgets(buffer, sizeof buffer, g_state_digest)); // corrected presentation
  assert(row + "\n" == buffer);
  std::ifstream tags(options.state_digest + ".presented-frontier.csv");
  assert(std::getline(tags, header));
  assert(std::getline(tags, row) && row == "5,1,100,99");
  assert(std::getline(tags, row) && row == "6,1,100,100");
  assert(!std::getline(tags, tail));
  std::ifstream late(options.state_digest + ".late.csv");
  assert(std::getline(late, header) && header == "body,match_frame,confirmed_match_frame,frontier_at_confirm");
  assert(!std::getline(late, tail));
  assert(logged.find("late resims with the same state") != std::string::npos);
  assert(logged.find("new bodies on a held match frame") != std::string::npos);
  fclose(g_state_digest); fclose(g_squad_confirmed); fclose(g_squad_presented_frontier); fclose(g_squad_late);
}
'''
        self.run_cpp(source)

    def test_extracted_game_observer_is_read_only_and_harness_only(self):
        game = read(ROOT / 'sourceport/game/shim/mu_online.c')
        self.run_cpp(r'''
#include "mu_squad_wire.h"
#include <cassert>
#include <cstdint>
#include <cstring>
using u8 = uint8_t; using u32 = uint32_t;
enum { RXB_SMALLEST_LATEST = 38 };
struct Online { int test, active, disconnected; u8 rxb[42]; } mu_online;
bool chosen = true;
int calls = 0; u8 captured[9];
int gmSquad_OnlineChosen() { return chosen; }
int global_frame() { return 101; }
void put32(u8* p, u32 v) { for (int i=3; i>=0; --i) { p[i]=v; v>>=8; } }
u32 be32(const u8* p) { return u32(p[0])<<24 | u32(p[1])<<16 | u32(p[2])<<8 | p[3]; }
int mu_online_abi_command(unsigned int cmd, const u8* p, unsigned size, u8* response,
                           unsigned capacity, unsigned* got) {
  assert(cmd == MU_SQUAD_COMMAND && size == 9 && !response && !capacity && !got);
  ++calls; memcpy(captured, p, size); return 0;
}
''' + function(game, 'squad_digest_observe') + r'''
int main(int, char**) {
  squad_digest_observe(0); assert(!calls);
  mu_online.test = 1; squad_digest_observe(0); assert(!calls);
  mu_online.active = 1; chosen = false; squad_digest_observe(0); assert(!calls);
  chosen = true; put32(mu_online.rxb + RXB_SMALLEST_LATEST, 99);
  const Online before = mu_online;
  squad_digest_observe(0);
  assert(!memcmp(&before, &mu_online, sizeof before));
  assert(calls == 1 && captured[0] == MU_SQUAD_OP_DIGEST_FRAME);
  assert(be32(captured+1) == 100 && be32(captured+5) == 99);
  squad_digest_observe(1); assert(captured[0] == MU_SQUAD_OP_DIGEST_END);
  mu_online.disconnected = 1; squad_digest_observe(1); assert(int32_t(be32(captured+5)) == -1);
}
''')

    def test_every_integration_is_squad_only_and_uses_remote_minimum_after_simulation(self):
        game = read(ROOT / 'sourceport/game/shim/mu_online.c')
        host = read(ROOT / 'port/runtime/host/host.cpp')
        app = read(ROOT / 'port/app/source_host.cpp')
        for text in (game, host, app, read(ROOT / 'port/runtime/host/host.h')):
            self.assertNotIn('squad_digest', without_squad(text))
        sample = function(game, 'squad_digest_observe')
        self.assertIn('RXB_SMALLEST_LATEST', sample)
        self.assertNotIn('YOUGAME_AGENT_TEST_HOOK', sample)
        self.assertIn('squad_digest_observe(0)', function(game, 'mu_online_frame_end'))
        self.assertIn('squad_digest_observe(0)', function(game, 'mu_online_scene_loop_exit'))
        self.assertIn('squad_digest_observe(1)', function(game, 'mu_online_match_exit'))
        self.assertIn('g_online_test_squad.count && slippi::Matchmaking::local_peer.enabled', app)
        self.assertIn('read_digest_state(state)', function(host, 'squad_digest_observe'))
        self.assertIn(CREDIT, read(ROOT / 'port/runtime/host/squad_digest.h'))


if __name__ == '__main__':
    unittest.main()
