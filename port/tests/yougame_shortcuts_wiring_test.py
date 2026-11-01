"""Offline source wiring for Windows-only shortcuts and the stdio settings hand-off."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class ShortcutsWiringTest(unittest.TestCase):
    def test_stdin_dispatch_precedes_platform_backlogs(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        embed = link.split('if (line.rfind("embed ", 0) == 0) {', 1)[1]
        dispatch = 'shortcuts::commands.dispatch(line.substr(6), [](const char* text) { host::log("%s", text); })'
        self.assertIn(dispatch, embed)
        self.assertLess(embed.index(dispatch), embed.index('#ifdef MELEE_LINUX_X11_EMBED'))
        self.assertLess(embed.index(dispatch), embed.index('box_backlog'))
        self.assertIn('#include "yougame_shortcuts.h"', link)

    def test_settings_uses_f1_atomic_and_consuming_path(self):
        window = source("port/runtime/host/window.cpp")
        self.assertIn('if (yougame::stdio_bridge() && yougame::shortcuts::commands.take_settings()) g_escape_press.store(true);', window)
        # Linux and the Mac SDL host pump their own window (port/linux/window_sdl.inl): the page's button must work there too.
        sdl = source("port/linux/window_sdl.inl")
        pump = sdl[sdl.index('void window_pump() {'):]
        self.assertIn('if (yougame::stdio_bridge() && yougame::shortcuts::commands.take_settings()) g_escape_press.store(true);', pump[:400])
        self.assertIn('if (w == VK_F1 && !(l & (1 << 30))) g_escape_press.store(true);', window)
        self.assertIn('bool window_take_escape() { return g_escape_press.exchange(false); }', window)
        header = source("port/runtime/hle/yougame_shortcuts.h")
        self.assertIn('if (command == "settings") { settings_.store(true); return true; }', header)
        self.assertIn('bool take_settings() { return settings_.exchange(false); }', header)

    def test_all_key_messages_precede_imgui_and_tab(self):
        window = source("port/runtime/host/window.cpp")
        key = window.index('if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)')
        imgui = window.index('if (g_on_message && g_on_message(h, m, w, l))')
        tab = window.index('if (m == WM_KEYDOWN && w == VK_TAB)')
        self.assertLess(key, tab)
        self.assertLess(tab, imgui)
        block = window[key:tab]
        self.assertIn('std::lock_guard<std::mutex> lock(g_keys_mutex);', block)
        for modifier in ('VK_SHIFT', 'VK_CONTROL', 'VK_MENU'):
            self.assertIn('GetKeyState(' + modifier + ')', block)
        self.assertIn('g_shortcut_keys.window_event(g_keys,', block)
        self.assertIn('m == WM_KEYDOWN || m == WM_SYSKEYDOWN, m == WM_SYSKEYDOWN || m == WM_SYSKEYUP,', block)
        self.assertIn('for (const char* line : event.lines) yougame::parity::signals.emit(line);', block)
        self.assertIn('if (m == WM_CHAR || m == WM_SYSCHAR)', block)
        self.assertIn('if (shortcut_swallowed(vk)) return 0;', block)
        self.assertIn('if (!(l & (1 << 30))) g_practice_toggle.store(true);', window[tab:imgui])

    def test_cancel_all_window_lifecycle_events(self):
        window = source("port/runtime/host/window.cpp")
        cancel = window.split('if (m == WM_KILLFOCUS ||', 1)[1].split('if (m == WM_KEYDOWN', 1)[0]
        for trigger in ('WM_ACTIVATE', 'WA_INACTIVE', 'WM_ACTIVATEAPP', 'WM_CLOSE', 'WM_DESTROY'):
            self.assertIn(trigger, cancel)
        self.assertIn('g_shortcut_keys.cancel().lines', cancel)
        destroy = window.split('void window_destroy() {', 1)[1].split('void window_pump()', 1)[0]
        self.assertIn('g_shortcut_keys.cancel().lines', destroy)

    def test_poll_capture_and_debug_filter_reserved_keys(self):
        window = source("port/runtime/host/window.cpp")
        self.assertIn('if (shortcut_swallowed(vk)) return false;', window)
        capture = window.split('bool input_poll_capture(', 1)[1].split('// Only controllers', 1)[0]
        self.assertIn('&& !shortcut_swallowed(vk)', capture)
        debug = window.split('void input_debug_snapshot(', 1)[1]
        self.assertIn('if (shortcut_swallowed(vk)) return false;', debug)
        helper = window.split('bool shortcut_swallowed(', 1)[1].split('#else', 1)[0]
        self.assertNotIn('GetAsyncKeyState', helper)
        self.assertIn('g_shortcut_keys.swallows(yougame::stdio_bridge(), vk)', helper)

    def test_cache_release_before_imgui_and_non_destructive_reservation(self):
        window = source("port/runtime/host/window.cpp")
        key = window.index('if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)')
        imgui = window.index('if (g_on_message && g_on_message(h, m, w, l))')
        block = window[key:window.index('if (m == WM_CHAR || m == WM_SYSCHAR)', key)]
        self.assertIn('g_shortcut_keys.window_event(g_keys,', block)
        self.assertIn('if (event.swallowed) return 0;', block)
        self.assertNotIn('g_keys[', block)  # No destructive scan or swallowed-press erasure.
        self.assertLess(key, imgui)
        ordinary_down = window.index('case WM_KEYDOWN: {', imgui)
        self.assertIn('if (w < 256) g_keys[w] = true;', window[ordinary_down:window.index('case WM_KEYUP:', ordinary_down)])
        system_down = window.split('case WM_SYSKEYDOWN:', 1)[1].split('case WM_SYSCHAR:', 1)[0]
        self.assertIn('w == VK_RETURN', system_down)
        self.assertNotIn('g_keys', system_down)
        self.assertIn('case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_KEYMENU) return 0;', window)

    def test_collision_log_names_action_at_start_and_config_change(self):
        window = source("port/runtime/host/window.cpp")
        apply = window.split('void apply_shortcuts() {', 1)[1].split('bool shortcut_swallowed(', 1)[0]
        self.assertIn('if (logged && config == last) return;', apply)
        self.assertIn('g_key_bindings.vk[i]', apply)
        self.assertIn('vk == config.menu.key || vk == config.chat.key', apply)
        self.assertIn('log("embed: shortcut overrides keyboard action %s (key %d)", actions[i], vk);', apply)
        create = window.split('void* window_create(', 1)[1].split('namespace {', 1)[0]
        self.assertIn('apply_shortcuts();', create)

    def test_every_caps_variant_and_chat_stdout(self):
        parity = source("port/runtime/hle/yougame_parity.h")
        variants = re.findall(r'k(?:Toplevel)?Caps = ("[^\n]+");', parity)
        self.assertEqual(len(variants), 3)
        for variant in variants:
            caps = json.loads(json.loads(variant)[3:])["caps"]
            self.assertEqual(caps.count('chat'), 1)
            self.assertEqual(caps.count('settings'), 1)
        header = source("port/runtime/hle/yougame_shortcuts.h")
        for event in ('down', 'up', 'cancel'):
            self.assertIn('"yg {\\"chat\\":\\"' + event + '\\"}"', header)

    def test_settings_hint_and_standalone_strings(self):
        parity = source("port/runtime/hle/yougame_parity.h")
        self.assertIn('kSettingsHint = "F1: game settings"', parity)
        settings = source("port/runtime/gx/pc_settings.cpp")
        self.assertIn('yougame::parity::kSettingsHint : "F1, or Start + D-pad Down + Z on a GameCube controller, opens settings."', settings)
        self.assertIn('yougame::parity::kSettingsHint : "Settings: F1  /  START + DOWN + Z"', settings)
        # Keep the separate app/menu hint used after matches.
        self.assertIn('kMenuHint = "Shift+Tab / YouGame menu (D-pad + Start)"', parity)

    def test_slippi_report_logs_only_error_count(self):
        report = source("port/runtime/hle/slippi_report.cpp")
        line = next(line for line in report.splitlines() if 'errors && errors->is_array()' in line)
        self.assertIn('host::log("slippi report: server returned %zu errors", errors->size())', line)
        self.assertNotIn('dump', line)
        self.assertNotIn('server error: %s', report)

    def test_linux_build_registers_both_cpp_variants_before_return(self):
        cmake = source("CMakeLists.txt")
        self.assertLess(cmake.index('port_yougame_shortcuts_${variant}_test'), cmake.index('  return()'))
        linux = source("tools/build_linux.py")
        self.assertIn('port_yougame_shortcuts_off_test', linux)
        self.assertIn('port_yougame_shortcuts_hook_test', linux)


if __name__ == '__main__':
    unittest.main()
