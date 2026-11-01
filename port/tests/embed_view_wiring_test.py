"""Embed view routing/capabilities without launching a native window or game."""
from pathlib import Path
import json
import re
import unittest

# Include the Mac contract in the existing CTest wiring entry.
from mac_embed_view_wiring_test import MacEmbedViewTest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class EmbedViewWiringTest(unittest.TestCase):
    def test_windows_posts_view_to_geometry_helper(self):
        window = source("port/runtime/host/window.cpp")
        self.assertIn('#include "yougame_embed_view.h"', window)
        self.assertRegex(window, r'sscanf\(line.c_str\(\), "view %d %d %d %d"[^;]+PostMessageW\(window, kBoxView,')
        self.assertRegex(window, r'case kBoxRect: case kBoxView:\s*apply_embed_geometry\(h, w, l, m == kBoxView\);')
        geometry = window.split('void apply_embed_geometry(', 1)[1].split('LRESULT CALLBACK', 1)[0]
        self.assertIn('yg_embed_view_update(&g_embed_view, box, is_view)', geometry)
        self.assertIn('SetWindowRgn(h, region, TRUE)', geometry)
        self.assertIn('result.resize ? 0 : SWP_NOSIZE', geometry)
        self.assertNotIn('g_on_resize', geometry)
        raise_fn = window.split('void raise_embed(HWND h) {', 1)[1].split('}', 1)[0]
        self.assertIn('SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)', raise_fn)
        self.assertIn('IsWindowVisible(h)', raise_fn)
        self.assertEqual(geometry.count('raise_embed(h);'), 2)
        self.assertIn('raise_embed(g_hwnd); log("window: in the YouGame player box");', window)
        self.assertRegex(window, r'(?s)case kBoxShow: \{.*?ShowWindow\(h, visible \? SW_SHOWNA : SW_HIDE\);\s*raise_embed\(h\);\s*return 0;\s*\}')
        self.assertNotIn('ResizeBuffers', geometry)
        self.assertIn('if (g_embed_view.has_view && g_client_w == LOWORD(l) && g_client_h == HIWORD(l)) return 0;', window)
        self.assertIn('if (c.rfind("view", 0) == 0) return std::string("view");',
                      source("port/runtime/hle/yougame_link.cpp"))

    def test_linux_routes_view_and_preserves_empty_input_shape(self):
        helper = source("port/linux/embed/yougame_embed_x11.c")
        self.assertIn('#include "../../runtime/hle/yougame_embed_view.h"', helper)
        self.assertIn('!strcmp(args[0], "rect") || !strcmp(args[0], "view")', helper)
        self.assertIn('const int is_view = !strcmp(args[0], "view");', helper)
        self.assertIn('yg_embed_view_update(&e->layout, box, is_view)', helper)
        self.assertIn('if (result.resize) XResizeWindow(', helper)
        self.assertRegex(helper, r'XShapeCombineRectangles\(e->dpy, e->game, ShapeBounding, 0, 0,\s*&clip, 1, ShapeSet, Unsorted\);')
        self.assertIn('XShapeCombineRectangles(dpy, game, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);', helper)
        sdl = source("port/linux/window_sdl.inl")
        self.assertIn('yg_embed_x11_command(g_x11_embed, line.c_str())', sdl)
        self.assertIn('command == "rect" || command == "view"', sdl)
        self.assertIn('!yg_embed_x11_visible(g_x11_embed)', sdl)
        self.assertIn('yg_embed_x11_size(g_x11_embed, &w, &h)', sdl)
        self.assertIn('(w != g_client_w || h != g_client_h)', sdl)

    def test_caps_on_windows_linux_and_mac(self):
        header = source("port/runtime/hle/yougame_parity.h")
        match = re.search(r'#if defined\(_WIN32\) \|\| defined\(__linux__\)\s*'
                          r'inline constexpr const char\* kCaps = ("[^\n]+?");\s*'
                          r'#elif defined\(__APPLE__\)\s*'
                          r'inline constexpr const char\* kCaps = ("[^\n]+?");\s*'
                          r'#else\s*inline constexpr const char\* kCaps = ("[^\n]+?");\s*#endif', header)
        self.assertIsNotNone(match)
        supported, mac, other = (json.loads(json.loads(value)[3:])["caps"] for value in match.groups())
        self.assertEqual(supported, ["ready", "overlay", "join", "notice", "pads", "rumble", "chat", "settings", "view", "volume"])
        self.assertEqual(mac, ["ready", "overlay", "join", "notice", "pads", "rumble", "chat", "settings", "view", "volume", "screenshot"])
        self.assertEqual(other, ["ready", "overlay", "join", "notice", "pads", "rumble", "chat", "settings"])
        self.assertEqual(header.count('kCaps ='), 3)

    def test_proof_logs(self):
        for path in ("port/runtime/host/window.cpp", "port/linux/embed/yougame_embed_x11.c"):
            text = source(path)
            self.assertIn('embed: view %d %d %d %d', text)
            self.assertIn('embed: clip %d %d %d %d (no resize)', text)
            self.assertIn('embed: rect outside view; clamped to view', text)

    def test_volume_on_windows_and_linux(self):
        # Both platforms that advertise the volume cap act on embed volume.
        window = source("port/runtime/host/window.cpp")
        self.assertIn('const int volume = yg_embed_volume(line.c_str());', window)
        self.assertIn('audio_set_page_volume(volume);', window)
        self.assertIn('embed: got volume %d', window)
        self.assertIn('audio_set_page_volume(static_cast<int>(gain * 100 + 0.5));', source("port/linux/window_sdl.inl"))
        # A master, never the player's own volume: no embed path calls audio_set_volume.
        handler = window.index('yougame::set_box_handler([window]')
        self.assertNotIn('audio_set_volume(', window[handler:window.index('});', handler)])
        self.assertNotIn('audio_set_volume(', source("port/linux/window_sdl.inl"))

    def test_volume_on_mac(self):
        # Mac advertises volume too: its box handler acts on it before the inbox's commands.
        embed = source("port/mac/embed_mac.mm")
        self.assertIn('#include "../runtime/hle/yougame_embed_volume.h"', embed)
        handler = embed.split('yougame::set_box_handler([inbox]', 1)[1].split('parse_command(line)', 1)[0]
        self.assertIn('const int volume = yg_embed_volume(line.c_str());', handler)
        self.assertIn('host::audio_set_page_volume(volume);', handler)
        self.assertIn('embed: got volume %d', handler)
        # A master here too: the Mac embed never calls the player's own audio_set_volume.
        self.assertNotIn('audio_set_volume(', embed)

    def test_mac_app_nap_off_while_embedded(self):
        # The game is never the active app: without the activity, macOS throttled it in the app's full screen.
        embed = source("port/mac/embed_mac.mm")
        self.assertRegex(embed, r'NSActivityUserInitiatedAllowingIdleSystemSleep \| NSActivityIdleDisplaySleepDisabled \|\s*'
                                r'NSActivityLatencyCritical')
        self.assertIn('[NSProcessInfo.processInfo endActivity:surface->awake];', embed)
        self.assertIn('embed: panel occlusion %s', embed)


if __name__ == "__main__":
    unittest.main()
