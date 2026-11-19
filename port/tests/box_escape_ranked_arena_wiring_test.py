"""122.21: Esc closes the game's YouGame box (as Shift+Tab does) and no Esc of that press reaches the game; the
online Ranked and Unranked selects show no ARENA tab or "Push the hand here" box. Source wiring plus the real
BoxEscape compiled; no real-game flow certification."""
from pathlib import Path
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


BOX_ESCAPE = r'''
#include "yougame_parity.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
int main() {
  yougame::parity::BoxEscape esc;
  int takes = 0;
  bool box = true;
  auto take = [&] { ++takes; const bool was = box; box = false; return was; };
  // Box up: the press, its repeats and its release are all kept from everything else, one Back.
  CHECK(esc.key(true,false,take)); CHECK(esc.key(true,true,take)); CHECK(esc.key(true,true,take));
  CHECK(esc.key(false,false,take)); CHECK(takes == 1);
  // Box gone: the next Esc goes on to the game (the Esc menu), repeats and release too.
  CHECK(!esc.key(true,false,take)); CHECK(!esc.key(true,true,take)); CHECK(!esc.key(false,false,take));
  CHECK(takes == 2);
  // A release with no press it took (the box opened while Esc was held) is not swallowed.
  yougame::parity::BoxEscape late;
  CHECK(!late.key(false,false,take));
  std::puts("box Esc: one Back a press; press, repeats and release kept from the game");
}
'''


class BoxEscapeWiringTest(unittest.TestCase):
    def test_box_escape_compiles_and_swallows_the_whole_press(self):
        menu.OnlineMenuTest().compile_fixture('box-escape', BOX_ESCAPE)

    def test_window_routes_take_esc_before_every_other_owner(self):
        win = source('port/runtime/host/window.cpp')
        sdl = source('port/linux/window_sdl.inl')
        take = '[] { return yougame::sign_in_escape(); }'
        self.assertEqual(win.count(take), 1)
        self.assertEqual(sdl.count(take), 1)
        at = win.index(take)
        self.assertLess(at, win.index('  // One embedded Escape press'))
        self.assertLess(at, win.index('if (g_on_message &&'))
        self.assertIn('w == VK_ESCAPE && (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)',
                      win[at - 400:at])
        self.assertIn('g_keys[VK_ESCAPE] = false;\n    return 0;', win[at:at + 300])
        at = sdl.index(take)
        self.assertLess(at, sdl.index('yougame::parity::embedded_escape('))
        self.assertLess(at, sdl.index('if (g_on_message &&'))
        self.assertLess(at, sdl.index('// Escape must reach fullscreen handling'))
        self.assertIn('set_key(VK_ESCAPE, false);\n    return;', sdl[at:at + 300])
        self.assertIn('yougame::parity::BoxEscape g_box_escape;', win)

    def test_esc_is_back_only_while_the_box_is_drawn(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        body = link[link.index('bool sign_in_escape() {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('if (!g_signin_drawn.load() || !sign_in_showing()) return false;', body)
        self.assertIn('sign_in_press(signin::Button::Back);', body)   # what Shift+Tab presses (channel_menu_key)
        settings = source('port/runtime/gx/pc_settings.cpp')
        self.assertIn('const bool signin_drawn = !state.fill_window && !state.open && !state.menu_open && '
                      'signin_overlay::open();', settings)
        self.assertIn('yougame::sign_in_drawn(signin_drawn);', settings)
        # The frame's shared Esc check stays as the fallback.
        self.assertIn('host::window_take_escape()) signin_overlay::back();', settings)


class RankedArenaWiringTest(unittest.TestCase):
    def test_select_mode_rides_with_the_hand(self):
        css = source('sourceport/game/shim/mu_slippi_css.c')
        hand = css[css.index('void mu_slippi_css_hand('):]
        hand = hand[:hand.index('\n}\n')]
        self.assertIn('unsigned char payload[5];', hand)
        self.assertIn('payload[4] = (unsigned char) mode;', hand)
        self.assertIn('if (packed == last && mode == last_mode) {', hand)
        for call in ('strstr', 'strchr', 'snprintf'):
            self.assertNotIn(call + '(', hand)
        host = source('port/app/source_host.cpp')
        self.assertIn('c == CMD_ARENA_HAND && (n == 4 || n == 5)', host)
        self.assertIn('slippi::arena::set_css_mode(n == 5 ? p[4] : -1);', host)

    def test_ranked_and_unranked_hide_the_rooms_panel(self):
        arena = source('port/runtime/hle/slippi_arena.cpp')
        self.assertIn('return mode == Matchmaking::RANKED || mode == Matchmaking::UNRANKED;', arena)
        settings = source('port/runtime/gx/pc_settings.cpp')
        self.assertIn('slippi::arena::css_hand(&hand_x, &hand_y) &&\n'
                      '                       !slippi::arena::css_ranked_or_unranked())', settings)
        # The game's mode numbers and the host's agree (MU_SLP_MODE_* against Matchmaking::OnlinePlayMode).
        native = source('sourceport/game/include/mu_native.h')
        self.assertIn('enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT', native)


if __name__ == '__main__':
    unittest.main()
