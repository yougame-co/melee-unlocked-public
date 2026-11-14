"""The Doubles home's pair colour (build 96, 11-11 report item 2).

Melee's team box comes back on the Doubles select, on the doors this machine plays: its own door and Player 2's
(sourceport/game/include/mu_friends4_css.inc mu_f4_bounds). A press there picks the pair's colour (red, blue or
green) and every door of the pair wears it (mu_f4_pair_team): Player 2 and a CPU partner at once, a friend
partner's machine when its view shows the change. The host's machine holds the colour: the guest wears the host's
(on first sight too), the host takes the guest's presses in the order they reach it, the last one stands, and the
host passes over a colour it wore itself in the last second, which may be the guest still showing it. The colour
goes out in the pick and hover the select already sends, the lock-in sends the colour the door wears, and the
match plays each room team in its lowest seat's colour (yougame_lobby_setup.h
doubles_colors, port/tests/yougame_doubles_lobby_test.cpp). mu_f4_pair_team is compiled from the select's own code
and run here on two stub machines joined by a delayed view; mu_f4_bounds is pinned by source.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INC = 'sourceport/game/include/mu_friends4_css.inc'


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig", errors="replace")


def body(source, name):
    at = source.index(name)
    at = source.index('{', at)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end]


def function(source, decl):
    return decl + body(source, decl) + '\n'


MACHINE = r'''
namespace NS {
struct CSSDoor { u8 p_kind = Gm_PKind_NA, team = 0; };
struct { CSSDoor doors[4]; } mnCharSel_803F0DFC;
struct PlayerInitData { u8 team = 0; };
struct CSSData { struct { struct { PlayerInitData players[4]; } start; } vs; };
CSSData css_data;
int redraws;
void mnCharSel_8025DB34(u8) { ++redraws; }
struct {
    MuLobbyView view{}; int view_ok = 1;
    unsigned char pair_team = 0xFF, partner_team_seen = 0xFF; int pair_wait = 0; unsigned char pair_seen[2] = {0xFF, 0xFF}, pair_worn[3] = {0, 0, 0};
    unsigned char sent[4] = {0xFF, 0, 0, 0}; unsigned char pair_unsent = 0; int pair_shown = 0;
} mu_f4;
int active = 1;   // the player hovers a fighter or has picked: the select's pick or hover is out (mu_f4_local)
int mu_f4_doubles() { return mu_f4.view_ok && (mu_f4.view.mode & MU_LOBBY_M_DOUBLES); }
int mu_f4_searching() { return mu_f4_doubles() && (mu_f4.view.mode & MU_LOBBY_M_SEARCHING); }
int mu_f4_host() { return (mu_f4.view.flags & MU_LOBBY_F_HOST) && !(mu_f4.view.flags & MU_LOBBY_F_STARTED); }
void OSReport(const char*, ...) {}
CODE
void visit(int seat) {   // a new visit of the select (mu_friends4_css_build's reset), seated at `seat`
    std::memset(&mu_f4.view, 0, sizeof mu_f4.view);
    mu_f4.view.flags = MU_LOBBY_F_ARMED | MU_LOBBY_F_KNOWN | (seat == 0 ? MU_LOBBY_F_HOST : 0);
    mu_f4.view.mode = MU_LOBBY_M_DOUBLES; mu_f4.view.my_seat = (unsigned char) seat;
    mu_f4.view.seats[seat].kind = MU_LOBBY_KIND_HUMAN;
    mu_f4.pair_team = mu_f4.partner_team_seen = mu_f4.pair_seen[0] = mu_f4.pair_seen[1] = 0xFF;
    mu_f4.pair_wait = 0; std::memset(mu_f4.pair_worn, 0, sizeof mu_f4.pair_worn);
    mu_f4.sent[0] = 0xFF; mu_f4.pair_unsent = 0; mu_f4.pair_shown = 0; active = 1;
    for (int i = 0; i < 4; ++i) { mnCharSel_803F0DFC.doors[i] = CSSDoor{}; mnCharSel_803F0DFC.doors[i].team = (u8) (i & 1); }
    mnCharSel_803F0DFC.doors[0].p_kind = mnCharSel_803F0DFC.doors[1].p_kind = Gm_PKind_Human;
}
CSSDoor& door(int i) { return mnCharSel_803F0DFC.doors[i]; }
void press(int i) { door(i).team = (u8) ((door(i).team + 1) % 3); css_data.vs.start.players[i].team = door(i).team; }
void press_to(int i, unsigned c) { while (door(i).team != c) press(i); }   // presses within one frame
// What mu_f4_local sends for this machine's own door: a pick carrying the door's colour while the player is on a
// fighter, nothing while the coin is held off the icons.
void local() {
    if (active) { mu_f4.sent[0] = MU_LOBBY_OP_PICK; mu_f4.sent[3] = door(mu_f4.view.my_seat).team; }
    else if (mu_f4.sent[0] != 0xFF) mu_f4.sent[0] = MU_F4_IDLE;
}
// One select frame in mu_friends4_css_frame's order: the pair's colour, the local sends, then the friend's door
// painted after the view drew it.
void frame() { mu_f4_pair_team(&css_data, mu_f4.view.my_seat); local(); mu_f4_pair_paint(&css_data, mu_f4.view.my_seat); }
}
'''

HARNESS = r'''#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include "mu_lobby_wire.h"
using u8 = unsigned char;
enum { Gm_PKind_Human = 0, Gm_PKind_Cpu = 1, Gm_PKind_NA = 3 };
#define MU_F4_HOLD 60
#define MU_F4_IDLE 0xFE
MACHINE_A
MACHINE_B
// The room: what each machine's select sent (its pick, team included) is the seat the room keeps, and it reaches
// the other's view `lag` frames later, as the room's broadcast does. Nothing sent: the room keeps the last.
struct Seat { unsigned char flags = 0, team = 0; };
struct Wire { std::deque<Seat> to_a, to_b; Seat a, b; } wire;
int lag = 5;
void tick() {
    A::frame(); B::frame();
    if (A::mu_f4.sent[0] == MU_LOBBY_OP_PICK) wire.a = Seat{MU_LOBBY_S_PICKED, A::mu_f4.sent[3]};
    if (B::mu_f4.sent[0] == MU_LOBBY_OP_PICK) wire.b = Seat{MU_LOBBY_S_PICKED, B::mu_f4.sent[3]};
    wire.to_b.push_back(wire.a); wire.to_a.push_back(wire.b);
    if ((int) wire.to_b.size() > lag) {
        B::mu_f4.view.seats[0].flags = wire.to_b.front().flags; B::mu_f4.view.seats[0].pick_team = wire.to_b.front().team; wire.to_b.pop_front();
    }
    if ((int) wire.to_a.size() > lag) {
        A::mu_f4.view.seats[1].flags = wire.to_a.front().flags; A::mu_f4.view.seats[1].pick_team = wire.to_a.front().team; wire.to_a.pop_front();
    }
}
void settle() { for (int i = 0; i < 300; ++i) tick(); }
void friends() {   // host A at seat 0, friend B at seat 1; each one's pick reaches the other `lag` frames later
    A::visit(0); B::visit(1); wire = Wire{};
    A::mu_f4.view.seats[1].kind = B::mu_f4.view.seats[0].kind = MU_LOBBY_KIND_HUMAN;
}
// Both machines wear one colour on both doors (the friend's door painted too), and the guest's view of the host
// has caught up.
bool agree(unsigned c) {
    return A::door(0).team == c && A::door(1).team == c && B::door(0).team == c && B::door(1).team == c &&
           A::mu_f4.pair_team == c && B::mu_f4.pair_team == c && B::mu_f4.view.seats[0].pick_team == c;
}
int main() {
    // A friend partner: on first sight the guest takes the host's colour (red), the host keeps it.
    friends(); settle(); assert(agree(0));
    // The host presses: blue on both machines.
    A::press(0); settle(); assert(agree(1));
    // The guest presses: green on both.
    B::press(1); settle(); assert(agree(2));
    // Nothing more redraws once they agree.
    { const int a = A::redraws, b = B::redraws; settle(); assert(A::redraws == a && B::redraws == b); }
    // Both press on the same frame (from red: the host to green, the guest to blue): the guest's reaches the host
    // last, so it stands.
    friends(); settle(); A::press(0); A::press(0); B::press(1); settle();
    assert(agree(1));
    // Crossing: the host picks blue and the guest green, the guest `gap` frames later (earlier when negative).
    // Whatever reaches the host last stands: the guest's when it arrives after the host's own press.
    for (lag = 1; lag <= 25; lag += 3)
        for (int gap = -lag - 4; gap <= 2 * lag + 4; ++gap) {
            friends(); settle();
            if (gap < 0) B::press_to(1, 2); else A::press_to(0, 1);
            for (int i = 0; i < (gap < 0 ? -gap : gap); ++i) tick();
            if (gap < 0) A::press_to(0, 1); else B::press_to(1, 2);
            settle();
            if (gap >= 0) assert(agree(2));
            else if (gap <= -lag - 2) assert(agree(1));
            else assert(agree(1) || agree(2));
        }
    lag = 5;
    // The guest cycles red -> blue -> green -> red faster than the host's second: it ends on red all the same
    // (the host waits out red, which it wore a moment ago, then takes it).
    friends(); settle(); B::press(1); tick(); tick(); B::press(1); tick(); tick(); B::press(1); settle(); assert(agree(0));
    // Presses far apart: the newest wins, whoever made it; two quick presses on either side land on the second.
    friends(); settle(); A::press(0); settle(); B::press(1); settle(); assert(agree(2));
    A::press(0); settle(); assert(agree(0));
    B::press(1); tick(); tick(); B::press(1); settle(); assert(agree(2));
    A::press(0); tick(); tick(); A::press(0); settle(); assert(agree(1));
    // Any presses, on either side, at any one-way lag up to 25 frames (a round trip under the host's 1 s): the two
    // machines end on one colour and stay there.
    unsigned seed = 1;
    auto rnd = [&](unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; };
    for (int run = 0; run < 400; ++run) {
        lag = 1 + (int) rnd(25);
        friends();
        for (int f = 0; f < 240; ++f) {
            if (rnd(20) == 0) A::press(0);
            if (rnd(20) == 0) B::press(1);
            tick();
        }
        settle(); settle();
        const unsigned c = A::door(0).team;
        assert(agree(c));
        const int a = A::redraws, b = B::redraws; settle(); assert(A::redraws == a && B::redraws == b && agree(c));
    }
    lag = 5;
    // The guest arrives wearing blue while the host wears green: green.
    friends(); A::press(0); A::press(0); settle(); assert(agree(2));
    // The guest presses with its coin in hand, off the icons: nothing goes out, and its door keeps the press (no
    // 2 s wait runs) until it is back on a fighter; then the host takes it.
    friends(); settle(); assert(agree(0));
    B::active = 0; tick(); B::press(1);
    for (int i = 0; i < 300; ++i) tick();
    assert(B::door(1).team == 1 && A::door(0).team == 0);
    B::active = 1; settle(); assert(agree(1));
    // The guest's first send carries a press (it wore the host's red while off the icons, then pressed blue): the
    // host has shown its colour for more than a second, so the first colour it sees is the guest's press.
    friends(); B::active = 0;
    for (int i = 0; i < 120; ++i) tick();
    assert(B::door(1).team == 0);
    B::press(1); for (int i = 0; i < 30; ++i) tick();
    B::active = 1; settle(); assert(agree(1));
    // A guest arriving with another colour (its door's last, blue) that it never pressed: it wears the host's red
    // before its first send, so red stays.
    friends(); B::active = 0;
    for (int i = 0; i < 120; ++i) tick();
    B::active = 1; settle(); assert(agree(0));
    // Before the host has shown any colour, the guest's first one sets the pair.
    friends(); A::active = 0;
    for (int i = 0; i < 120; ++i) tick();
    A::active = 1; settle(); assert(agree(1));
    // A match being set up: no colour moves.
    friends(); A::press(0); A::press(0); settle(); assert(agree(2));
    A::mu_f4.view.flags |= MU_LOBBY_F_STARTED; A::press(0); settle();
    assert(A::door(0).team == 0 && A::mu_f4.pair_team == 2);

    // Player 2 on the host's machine: one colour for both doors, either box picks it.
    A::visit(0);
    A::mu_f4.view.mode |= MU_LOBBY_M_LOCAL_OWNER; A::mu_f4.view.seats[1].kind = MU_LOBBY_KIND_LOCAL;
    assert(mu_lobby_local_owned(&A::mu_f4.view));
    A::door(1).p_kind = Gm_PKind_Human;
    A::frame();   // Player 2's door (blue by Melee's default) takes the pair's red; red stays
    assert(A::door(0).team == 0 && A::door(1).team == 0 && A::mu_f4.pair_team == 0);
    assert(A::css_data.vs.start.players[1].team == 0);
    A::press(1); A::frame(); assert(A::door(0).team == 1 && A::door(1).team == 1);   // P2's box: blue
    A::press(0); A::frame(); assert(A::door(0).team == 2 && A::door(1).team == 2);   // P1's box: green
    assert(A::css_data.vs.start.players[0].team == 2 && A::css_data.vs.start.players[1].team == 2);
    // Player 2 leaves and sits down again later: the door takes the pair's colour, not the other way round.
    A::mu_f4.view.mode &= ~MU_LOBBY_M_LOCAL_OWNER; A::mu_f4.view.seats[1].kind = MU_LOBBY_KIND_OPEN; A::frame();
    A::door(1).team = 1;
    A::mu_f4.view.mode |= MU_LOBBY_M_LOCAL_OWNER; A::mu_f4.view.seats[1].kind = MU_LOBBY_KIND_LOCAL; A::frame();
    assert(A::door(0).team == 2 && A::door(1).team == 2 && A::mu_f4.pair_team == 2);

    // A CPU partner follows the host's box; the view redrawing the CPU's door never moves the pair.
    A::visit(0);
    A::mu_f4.view.seats[1].kind = MU_LOBBY_KIND_CPU; A::door(1).p_kind = Gm_PKind_Cpu;
    A::frame(); assert(A::door(1).team == 0);
    A::press(0); A::frame(); assert(A::door(0).team == 1 && A::door(1).team == 1);
    A::door(1).team = 0; A::frame();   // a redraw from a view that hasn't caught up
    assert(A::door(0).team == 1 && A::door(1).team == 1 && A::mu_f4.pair_team == 1);
    std::puts("ok");
    return 0;
}
'''


class PairColourHarness(unittest.TestCase):
    """mu_f4_pair_team from the select's own code, on two machines and on one with Player 2 or a CPU."""

    def test_pair_colour(self):
        if shutil.which('c++') is None:
            self.skipTest('no C++ compiler')
        inc = read(INC)
        code = function(inc, 'static void mu_f4_pair_team(CSSData* css, int mine)')
        paint = 'static void mu_f4_pair_paint(CSSData* css, int mine)'
        code += function(inc, paint) if paint in inc else paint + ' { (void) css; (void) mine; }\n'
        machine = MACHINE.replace('CODE', code)
        source = HARNESS.replace('MACHINE_A', machine.replace('NS', 'A')).replace('MACHINE_B', machine.replace('NS', 'B'))
        with tempfile.TemporaryDirectory(prefix='pair-colour-') as d:
            cpp, exe = Path(d) / 'harness.cpp', Path(d) / 'harness'
            cpp.write_text(source)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wno-unused-function',
                                     '-I' + str(ROOT / 'port/runtime/abi'), str(cpp), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
            self.assertIn('ok', run.stdout)


class PairColourWiring(unittest.TestCase):
    def setUp(self):
        self.inc = read(INC)

    def test_team_box_on_this_machines_doors_in_doubles(self):
        bounds = body(self.inc, 'static void mu_f4_bounds(int mine)')
        self.assertIn('const int own = i == mine || (i == 1 && mine == 0 && mu_f4.view_ok && '
                      'mu_lobby_local_owned(&mu_f4.view));', bounds)
        self.assertIn('const int teambox = doubles ? own && !locked && !(mu_f4.view.flags & MU_LOBBY_F_STARTED)\n'
                      '                                    : i == mine || (host && kind == MU_LOBBY_KIND_CPU);', bounds)

    def test_pair_colour_runs_each_frame_before_any_pick_goes_out(self):
        # The pair's colour is settled before this machine's hover or pick (so a press goes out the same frame) and
        # before the host sends the CPU's; the friend's door is painted after the view drew it.
        frame = self.inc[self.inc.index('        mu_f4_pair_team(css, mine);    /* first'):]
        self.assertLess(frame.index('mu_f4_pair_team(css, mine);'), frame.index('mu_f4_local(css, mine);'))
        self.assertLess(frame.index('mu_f4_doors(css, mine);'), frame.index('mu_f4_pair_paint(css, mine);'))
        self.assertLess(frame.index('mu_f4_partner_sync(css, mine);'), frame.index('mu_f4_pair_paint(css, mine);'))
        self.assertLess(frame.index('mu_f4_pair_paint(css, mine);'), frame.index('mu_f4_host_doors(css, mine);'))

    def test_a_new_visit_starts_with_no_pair_colour(self):
        build = body(self.inc, 'static void mu_friends4_css_build(void)')
        self.assertIn('mu_f4.pair_team = mu_f4.partner_team_seen = mu_f4.pair_seen[0] = mu_f4.pair_seen[1] = 0xFF;',
                      build)

    def test_the_colour_goes_out_in_the_pick_and_the_lock_in(self):
        local = body(self.inc, 'static void mu_f4_local(CSSData* css, int mine)')
        self.assertIn('req[3] = mu_f4_u8(door->team, MU_LOBBY_TEAM_MAX);', local)
        lock = body(self.inc, 'static void mu_f4_lock_in(int mine)')
        self.assertIn('sel.team_id = mu_f4_doubles() && mnCharSel_803F0DFC.doors[mine].team <= MU_LOBBY_TEAM_MAX\n'
                      '                          ? mnCharSel_803F0DFC.doors[mine].team : me->pick_team;', lock)


if __name__ == '__main__':
    unittest.main()
