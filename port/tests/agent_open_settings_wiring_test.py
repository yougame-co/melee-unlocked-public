"""Agent Video startup reuses the screenshot tab route after a presented game frame."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def guards_at(text, marker):
    guards = []
    for line in text[:text.index(marker)].splitlines():
        if line.lstrip().startswith('#if'):
            guards.append(line.strip())
        elif line.lstrip().startswith('#endif'):
            guards.pop()
    return guards


class AgentOpenSettingsWiringTest(unittest.TestCase):
    def test_click_publish_reset_and_session_are_hook_only(self):
        header = source('port/runtime/hle/yougame_agent_test.h')
        for marker in ('inline std::atomic<int> ui_click_frame', 'UiClick ui_click;',
                       'if (key == "uiClick")', 'ui_click_frame.store(0);',
                       'ui_click_frame.store(out.ui_click.frame, std::memory_order_release);'):
            self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK', guards_at(header, marker))
        session = header.split('const bool active_session =', 1)[1].split('if (!active_session)', 1)[0]
        self.assertIn('out.ui_click.frame > 0', session)
        publish = header.split('ui_click_x.store(out.ui_click.x);', 1)[1]
        self.assertLess(publish.index('ui_click_y.store'), publish.index('ui_click_frame.store'))

    def test_click_follows_backend_and_scale_before_imgui_frame(self):
        ui = source('port/runtime/gx/pc_settings.cpp').split('bool settings_frame(', 1)[1]
        gate = 'if (!state.fill_window && yougame::parity::signals.first_present(first_present))\n    ++agent_settings_frame;'
        self.assertIn(gate, ui)
        inject = ui.index('settings_input::click(')
        for marker in ('ImGui_ImplSDL2_NewFrame();', 'ImGui_ImplWin32_NewFrame();', 'g_frame_ui_scale.store(ui_scale,'):
            self.assertLess(ui.index(marker), inject)
        self.assertLess(inject, ui.index('ImGui::NewFrame();'))
        self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK', guards_at(ui, 'settings_input::click('))
        self.assertIn('frame > 0 && frame == agent_settings_frame', ui)
        self.assertIn('ui_click_frame.store(0);', ui)
        self.assertIn('std::chrono::steady_clock::now().time_since_epoch()', ui)
        diag = ui.split('const ImGuiIO& diag_io =', 1)[1].split('if (launcher_old_look)', 1)[0]
        self.assertIn('agent_settings_frame <= agent_click_diag_until', diag)
        self.assertIn('settings_input::click_event_seen(agent_click.down)', diag)
        self.assertIn('settings_input::click_event_seen(agent_click.up)', diag)
        self.assertIn('if (agent_click_up_seen) agent_click_diag_until = 0;', diag)
        self.assertIn('nav id %08X frame %d', diag)
        self.assertLess(diag.index('if (agent_click_down_seen'), diag.index('if (g_ui_diag'))

    def test_parser_publish_reset_and_session_are_hook_only(self):
        header = source('port/runtime/hle/yougame_agent_test.h')
        for marker in ('inline std::atomic<bool> open_settings_video', 'bool open_settings_video = false;',
                       'if (key == "openSettings")', 'open_settings_video.store(false);',
                       'open_settings_video.store(out.open_settings_video);'):
            self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK', guards_at(header, marker))
        parser = header.split('if (key == "openSettings")', 1)[1].split('if (key == "bootMatch")', 1)[0]
        self.assertIn('value.is_string() && value == "video"', parser)
        self.assertIn('applied("openSettings")', parser)
        self.assertIn('agent-test: openSettings invalid value; ignored', parser)
        self.assertNotIn('Engine::', parser)
        session = header.split('const bool active_session =', 1)[1].split('if (!active_session)', 1)[0]
        self.assertIn('out.open_settings_video', session)

    def test_ui_waits_then_uses_existing_startup_tab_route(self):
        ui = source('port/runtime/gx/pc_settings.cpp')
        consume = ui.split('yougame::parity::Signals::Clock::time_point first_present;', 1)[1].split('#endif', 1)[0]
        self.assertIn('!state.fill_window && yougame::parity::signals.first_present(first_present)', consume)
        self.assertIn('yougame::agent_test::open_settings_video.exchange(false)', consume)
        self.assertIn('state.open = true;', consume)
        self.assertIn('agent_open_video = true;', consume)
        self.assertIn('host::log("agent-test: openSettings video");', consume)
        self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK', guards_at(ui, 'open_settings_video.exchange(false)'))
        startup = ui.split('static bool screenshot_tab_set = false;', 1)[1].split('const bool clean_home', 1)[0]
        self.assertIn('|| agent_open_video', startup)
        self.assertIn('if (agent_open_video) requested = "Video";', startup)
        self.assertIn('std::getenv("MELEE_SETTINGS_TAB")', startup)
        self.assertIn('"Video", "Audio", "Game"', startup)
        self.assertEqual(ui.count('if (std::strcmp(requested, tabs[i]) == 0)'), 1)
        self.assertIn('state.active_tab = i;', startup)
        for style in ('clean', 'dashboard', 'radial', 'gd', 'wide'):
            self.assertIn(f'state.{style}_detail_open = true;', startup)


if __name__ == '__main__':
    unittest.main()
