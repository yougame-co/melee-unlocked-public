"""Fullscreen stays on the reader/render threads and out of saved options."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class EmbedFullscreenWiringTest(unittest.TestCase):
    def test_every_first_caps_line_advertises_fullscreen(self):
        parity = source("port/runtime/hle/yougame_parity.h")
        caps = re.findall(r'const char\* k(?:Toplevel)?Caps = ("[^\n]+?");', parity)
        self.assertEqual(len(caps), 3)
        for literal in caps:
            self.assertIn("fullscreen", json.loads(json.loads(literal)[3:])["caps"])
        start = parity.split("void start(", 1)[1].split("int sync_match(", 1)[0]
        self.assertIn("writer(caps(toplevel)); writer_.store(writer);", start)

    def test_reader_consumes_before_platform_routing(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        handler = link.split('if (line.rfind("embed ", 0) == 0) {', 1)[1]
        shared = handler.split("#ifdef MELEE_LINUX_X11_EMBED", 1)[0]
        self.assertLess(shared.index("shortcuts::commands.dispatch"), shared.index("embed_fullscreen::state.receive"))
        self.assertIn("embed_fullscreen::state.receive(std::string_view(line).substr(6))", shared)
        self.assertIn("fullscreen != embed_fullscreen::Line::Other", shared)
        self.assertIn('yougame: embed fullscreen %d (display option now %s)', shared)
        self.assertIn('on ? "Borderless fullscreen" : "Window"', shared)
        self.assertIn('if (!invalid_logged) host::log("yougame: embed fullscreen invalid value; ignored");', shared)
        self.assertIn("return;", shared)
        for forbidden in ("window_", "signals.emit", "state.request", "options."):
            self.assertNotIn(forbidden, shared)
        header = source("port/runtime/hle/yougame_embed_fullscreen.h")
        self.assertIn('command != "fullscreen 0" && command != "fullscreen 1"', header)
        self.assertIn("std::atomic<uint64_t> app_{0}", header)

    def test_settings_gate_and_only_player_change_requests(self):
        settings = source("port/runtime/gx/pc_settings.cpp")
        combo = settings.split('const char* displays[] =', 1)[1].split('// Widescreen:', 1)[0]
        self.assertIn('state.shown(yougame::embedded(), fullscreen_clock())', combo)
        self.assertIn('app_display >= 0 ? app_display : options.exclusive_fullscreen ? 2 : options.fullscreen ? 1 : 0', combo)
        player = combo.split('if (settings_combo("Display", &display, displays, 3)) {', 1)[1]
        embedded, fallback = player.split('} else {', 1)
        self.assertIn('if (app_display >= 0)', embedded)
        self.assertIn('state.request(yougame::embedded(), display != 0, fullscreen_clock())', embedded)
        for forbidden in ('options.fullscreen =', 'options.exclusive_fullscreen =', 'changed =', 'write_settings'):
            self.assertNotIn(forbidden, embedded)
        self.assertIn('options.fullscreen = display == 1;', fallback)
        self.assertIn('options.exclusive_fullscreen = display == 2;', fallback)
        self.assertIn('changed = true;', fallback)
        self.assertIn("Full screen is the YouGame app's window. Shift+Esc leaves.", combo)
        self.assertEqual(settings.count('embed_fullscreen::state.request('), 1)
        writer = settings.split('static bool write_settings_file_impl(const SettingsState& state, const RenderOptions& options) {', 1)[1].split('\n}\n', 1)[0]
        self.assertNotIn('embed_fullscreen', writer)
        self.assertIn('"\\nfullscreen " << options.fullscreen', writer)
        self.assertIn('"\\nexclusivefullscreen " << options.exclusive_fullscreen', writer)

    def test_flush_is_unconditional_at_frame_entry(self):
        settings = source("port/runtime/gx/pc_settings.cpp")
        entry = settings.split('bool settings_frame(SettingsState& state, RenderOptions& options) {', 1)[1].split('state.page_navigation.active', 1)[0]
        self.assertIn('state.flush(yougame::embedded(), fullscreen_clock())', entry)
        self.assertNotIn('state.open', entry)
        self.assertNotIn('#if', entry)
        self.assertIn('yougame: fullscreen request %d', entry)
        self.assertIn(r'"yg {\"fullscreen\":1}" : "yg {\"fullscreen\":0}"', entry)
        for path in ('port/runtime/gx/pc_settings.cpp', 'port/runtime/gx/pc_settings_dx11.cpp',
                     'port/linux/full/pc_settings_gl.cpp', 'port/runtime/gx/gx_vulkan.cpp'):
            self.assertIn('settings_frame(', source(path))

    def test_existing_native_fullscreen_guards_remain(self):
        self.assertIn('if (yougame::embedded()) return;', source('port/runtime/host/window.cpp'))
        for path in ('port/runtime/gx/gx_d3d11.cpp', 'port/runtime/gx/gx_d3d12.cpp'):
            self.assertRegex(source(path), r'yougame::embedded\(\)[^\n]*exclusive_fullscreen = false')
        sdl = source('port/linux/window_sdl.inl')
        self.assertIn('mac_embed', sdl)
        self.assertIn('g_x11_embed', sdl)


if __name__ == '__main__':
    unittest.main()
