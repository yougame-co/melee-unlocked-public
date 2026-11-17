"""Keep pacing observers and unchanged timing routines pinned across renderer upgrades."""
from pathlib import Path
import re
import difflib
import json
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = "2f4aa72"
GIT = "/Library/Developer/CommandLineTools/usr/bin/git" if sys.platform == "darwin" else "git"


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def baseline(path, revision=BASE):
    return subprocess.check_output([GIT, "show", f"{revision}:{path}"], cwd=ROOT).decode("utf-8-sig").replace("\r\n", "\n")


class PacingDiagnosticsWiringTest(unittest.TestCase):
    def test_pacing_routines_equal_baseline_after_removing_only_observers(self):
        for api in (11, 12):
            with self.subTest(api=api):
                path = f"port/runtime/gx/gx_d3d{api}.cpp"
                current = source(path)
                self.assertEqual(current.count("pacing_diagnostics_.present("), 1)
                self.assertEqual(current.count("pacing_diagnostics_.poll("), 1)
                self.assertEqual(current.count("pacing_diagnostics_.statistics("), 1)
                clean = current.replace('#include "pacing_diagnostics_win.h"\n', '')
                clean = clean.replace('  pacing_diag::WindowsRecorder pacing_diagnostics_;\n', '')
                clean = clean.replace('  HRESULT diagnostic_statistics_result = E_FAIL;\n', '')
                clean = clean.replace('SUCCEEDED(diagnostic_statistics_result = swapchain_->GetFrameStatistics(&st))',
                                      'SUCCEEDED(swapchain_->GetFrameStatistics(&st))')
                clean = clean.replace('  pacing_diagnostics_.statistics(diagnostic_statistics_result, st);\n', '')
                clean = re.sub(r' +pacing_diagnostics_\.present\(present_call, opts_\.vsync,\n +present\.interval, present\.flags, opts_\.fps_cap, fullscreen_state_\.exclusive_active\);\n', '', clean)
                clean = clean.replace('  pacing_diagnostics_.poll(swapchain_.Get(), hwnd_);\n', '')
                # Build 88's slow-frame log times the GPU wait (an observer: the wait itself is unchanged).
                clean = clean.replace('    Stopwatch gpu_wait_sw;\n', '')
                clean = clean.replace('    ++slow_.gpu_waits; slow_.gpu_wait_ms += gpu_wait_sw.lap() * 1e3;\n', '')
                # v0.9.96 rewords the variable-refresh log line only (the cap and V-Sync are unchanged here).
                new_line = '"display; frame cap below %.0f fps, V-Sync follows the player\'s setting"'
                self.assertEqual(clean.count(new_line), 1)
                clean = clean.replace(new_line, '"display; frames are presented synced under %.0f a second, so they never tear"')
                # v0.9.83 adds MSAA, shared presentation shaders, and occlusion handling.
                # Pin the unaffected pacing routines rather than freezing the entire renderer.
                from yougame_console_wiring_test import body
                names = ('gpu_frame_begin', 'gpu_frame_end', 'update_variable_refresh') if api == 11 else (
                    'wait_frame_latency', 'wait_gpu', 'update_variable_refresh')
                for name in names:
                    self.assertEqual(body(clean, name), body(baseline(path), name), name)

    def test_timestamp_and_actual_present_arguments_are_recorded_after_wait_measurement(self):
        for api in (11, 12):
            s = source(f"port/runtime/gx/gx_d3d{api}.cpp")
            call = s.index('pacing_diagnostics_.present(present_call, opts_.vsync,')
            self.assertGreater(call, s.index('return swapchain_->Present(present.interval, present.flags);'))
            self.assertGreater(call, s.index('present_wait_ += Stopwatch::now() - present_call;'))
            self.assertIn('present.interval, present.flags, opts_.fps_cap, fullscreen_state_.exclusive_active);', s[call:call+210])
            self.assertGreater(s.index('pacing_diagnostics_.poll('), s.index('  gpu_frame_end();') if api == 11 else s.index('queue_->Signal(fence_.Get(), fence_value_);', s.index('return swapchain_->Present(')))

    def test_no_new_frame_statistics_queries_or_vrr_changes(self):
        for api in (11, 12):
            path = f"port/runtime/gx/gx_d3d{api}.cpp"
            s = source(path)
            self.assertEqual(s.count('GetFrameStatistics('), baseline(path).count('GetFrameStatistics('))
            self.assertIn('if ((frames_presented_ & 31) == 0) update_variable_refresh();', s)
            self.assertIn('if (!vrr_readable_) { vrr_.restart(); return; }', s)
        win = source('port/runtime/gx/pacing_diagnostics_win.h')
        self.assertNotIn('->GetFrameStatistics(', win)
        self.assertIn('GetFrameStatisticsMedia(&media)', win)

    def test_timer_requests_and_masks_are_unchanged(self):
        path = 'port/app/main.cpp'
        s, old = source(path), baseline(path)
        for pattern in (r'(?<![\w:])time(?:Begin|End)Period\([^)]*\)',
                        r'SetProcessInformation\([^;\n]*?sizeof throttling\)',
                        r'throttling\.(?:ControlMask|StateMask|Version) = [^;]+;'):
            # The #else branch is the byte-for-byte original non-Windows fallback.
            normalized = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s).replace('#else\n      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);\n#endif', '#endif')
            self.assertEqual(re.findall(pattern, normalized), re.findall(pattern, re.sub(r'"(?:[^"\\]|\\.)*"', '""', old)))
        self.assertIn('if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling)) {\n#ifdef _WIN32\n      timer_resolution_error = GetLastError();', s)

    def test_log_file_selected_before_new_startup_lines(self):
        s = source('port/app/main.cpp')
        self.assertGreater(s.index('host::log("timing: timeBeginPeriod'), s.index('host::options.log_file = argv[++i]'))
        self.assertIn('TokenIsAppContainer', s)
        self.assertIn('CloseHandle(diagnostic_token);', s)

    def test_settings_event_is_windows_only_and_preserves_toggle(self):
        s = source('port/runtime/gx/pc_settings.cpp')
        # Build 60: the waiting pill is one row on the picture's bottom edge. Reverse it to its 60 px form first.
        pill_now = '''      // One row on the picture's bottom edge, sized to the picture: 60 px up it sat on the retail menus'
      // description box (VS. Mode, Name Entry) and on the CSS name cards.
      const float picture = std::min(screen.y, screen.x / std::max(0.1f, g_overlay_game_aspect));
      const float font = std::min(ImGui::GetFontSize(), std::max(8.0f, picture * 0.026f));
      ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, (screen.y + picture) * 0.5f - std::max(2.0f, picture * 0.006f)),
                              ImGuiCond_Always, ImVec2(0.5f, 1));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 0.6f, font * 0.3f));
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * font / ImGui::GetFontSize());
      ImGui::Begin("##yougame_friend_waiting", nullptr, passive_flags);
      screen_label::reserve_bottom(ImGui::GetWindowPos().y);
      ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str());
      if (status == yougame::friends::kWaiting || !console_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Z: cancel");
      }
      ImGui::End();
      ImGui::PopFont();
      ImGui::PopStyleVar();
'''
        pill_before = '''      ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 60), ImGuiCond_Always, ImVec2(0.5f, 1));
      ImGui::Begin("##yougame_friend_waiting", nullptr, passive_flags);
      ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str());
      if (status == yougame::friends::kWaiting || !console_status.empty()) ImGui::TextDisabled("Z: cancel");
      ImGui::End();
'''
        self.assertEqual(s.count(pill_now), 1)
        s = s.replace(pill_now, pill_before)
        # Doubles 64: the Doubles home draws its own box above the pill. Reverse exactly that block.
        box_start = s.index("#ifdef MELEE_DOUBLES\n    // The Doubles home's own box")
        box = s[box_start:s.index('    // Online menu/CSS visibility alone', box_start)]
        self.assertTrue(box.endswith('#endif\n') and box.count('ImGui::Begin(') == 1, box)
        s = s.replace(box, '', 1)
        addition = '''#ifdef _WIN32
    const bool diagnostic_vsync_before = options.vsync;
#endif
    changed |= settings_toggle("VSync (prevents tearing)", &options.vsync);
#ifdef _WIN32
    if (options.vsync != diagnostic_vsync_before)
      host::log("present: vsync %s (settings)", options.vsync ? "on" : "off");
#endif'''
        self.assertIn(addition, s)
        # Build 106 tells YouGame when MU's own menus are up (gameState "menu"): reverse its two hunks first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build106_gamestate_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 105 adds YouGame's Sign in by code panel (signin_overlay.cpp: drawn, captures input, Esc is
        # Back): reverse exactly its pc_settings.cpp hunks first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build105_signin_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 103 scales the F1/Esc menu bar with the window height and makes it near-opaque: reverse
        # exactly that block first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build103_f1menu_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 101 merges upstream v0.9.120 (Gecko function patches, Boot to CSS, Center 2P HUD): reverse exactly
        # its pc_settings.cpp hunks (v0.9.119 -> v0.9.120) first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build101_0120_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 100 merges upstream v0.9.119 (Hide fighter shadows, render latency on Vulkan NVIDIA): reverse
        # exactly its pc_settings.cpp hunks (v0.9.118 -> v0.9.119) first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build100_0119_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 96.3 merges upstream v0.9.116 (snapback filter size and time as separate settings): reverse
        # exactly its pc_settings.cpp hunks (v0.9.115 -> v0.9.116) first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build963_0116_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Builds 93-95 merge upstream v0.9.111-v0.9.113 (Lighting choices, the frame-protection live value
        # moved, and more): reverse exactly their pc_settings.cpp hunks (v0.9.110 -> v0.9.113) first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build95_0113_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 90 merges upstream v0.9.110 (shiny floors on stage mods, glowing stage art lights the fighters):
        # reverse exactly its pc_settings.cpp hunks (vs build 89) first.
        for approved in json.loads((ROOT / 'port/tests/fixtures/build90_0110_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 86 merges upstream v0.9.107 (path tracing on stage mods, effects light the stage): reverse exactly
        # its pc_settings.cpp hunks (vs build 85).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build86_0107_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Doubles 67 merges the shader cache saver and --prewarm-only (load_pc_settings returns before the
        # gameplay files in a graphics-only run; the graphics settings it needs move above that return):
        # reverse exactly its pc_settings.cpp hunks (vs Doubles 66).
        for approved in json.loads((ROOT / 'port/tests/fixtures' / 'doubles67_prewarm_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 66 merges upstream v0.9.105 (the launcher's settings window keeps path tracing on, the RTX
        # Brightness slider's own ID, the panel-nav test driver): reverse exactly its pc_settings.cpp hunks
        # (vs build 65).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build66_0105_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 65 merges upstream v0.9.104 (path tracing and HDR keys, AMD FSR upscaling, frame generation and
        # its download, pause-screen looks; in the YouGame app the FSR download and the one-click mod looks
        # are hidden): reverse exactly its pc_settings.cpp hunks (vs build 64).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build65_0104_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 58 merges upstream v0.9.102 (the full Fountain of Dreams reflections tooltip): reverse exactly
        # its pc_settings.cpp hunk (vs build 57).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build58_0102_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 56 merges upstream v0.9.99 and v0.9.100 (offline-only community codes: Instant Melee, the
        # unrestricted pause camera): reverse exactly its pc_settings.cpp hunks (vs build 55).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build56_0999_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 54 merges upstream v0.9.98 (the Practice menu follows the last pad, XeSS as its own
        # download, hidden in the YouGame app): reverse exactly its pc_settings.cpp hunks (vs build 53).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build54_0998_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 53 merges upstream v0.9.97 (community codes, Coming soon stickers, the V-Sync tooltip):
        # first reverse exactly its pc_settings.cpp hunks (vs build 52).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build53_0997_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 52 scripted mouse events and diagnostics.
        for changed in json.loads((ROOT / 'port/tests/fixtures/build52_settings_ranges.json').read_text()):
            self.assertEqual(s.count(changed['after']), 1, changed['after'])
            s = s.replace(changed['after'], changed['before'], 1)
        # Build 51 fullscreen and agent settings hooks: reverse only these exact ranges.
        for changed in json.loads((ROOT / 'port/tests/fixtures/build51_settings_ranges.json').read_text()):
            self.assertEqual(s.count(changed['after']), 1, changed['after'])
            s = s.replace(changed['after'], changed['before'], 1)
        # Build 50m merges upstream v0.9.96: first reverse exactly its pc_settings.cpp hunks (vs v0.9.95).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build50m_0996_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 49.2 merges upstream v0.9.95: first reverse exactly its pc_settings.cpp hunks (vs v0.9.94).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build492_0995_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 49 merges upstream v0.9.94: first reverse exactly its pc_settings.cpp hunks (vs v0.9.93).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build49_0994_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 44 merges upstream v0.9.91: first reverse exactly its pc_settings.cpp hunks (vs v0.9.89).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build44_0991_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 42 merges upstream v0.9.89: first reverse exactly its pc_settings.cpp hunks (vs 0565f039).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build42_0989_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Build 39 merges upstream v0.9.88: first reverse exactly its pc_settings.cpp hunks (vs 8c6b7cb2).
        for approved in json.loads((ROOT / 'port/tests/fixtures/build39_0988_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Round 27 update notice and Round 30 menu safety fixes: reverse only exact reviewed ranges.
        for approved in json.loads((ROOT / 'port/tests/fixtures/round27_settings_ranges.json').read_text()):
            self.assertEqual(s.count(approved['after']), 1)
            s = s.replace(approved['after'], approved['before'], 1)
        # Round 24 admits only these literal, reviewed settings edits, including label observers.
        # Reversing each exact byte range retains the existing whole-file pacing comparison.
        ranges = json.loads((ROOT / 'port/tests/fixtures/round24_settings_ranges.json').read_text())
        for approved in ranges:
            self.assertEqual(s.count(approved['after']), 1, approved['after'])
            s = s.replace(approved['after'], approved['before'], 1)
        # Round 23 changes only the FPS-default log and overlay navigation here.
        # Reverse these exact approved ranges; keep the whole-file pacing pin.
        for old, new in (
            ('    host::log("settings: vsync %s%s, fps counter %s (YouGame)", options.vsync ? "on" : "off",\n'
             '              g_display_keys.own_vsync && saved_vsync_yougame < 0 ? " (YouGame default)" : "", options.show_fps ? "on" : "off");',
             '    host::log("settings: vsync %s%s, fps counter %s (%s)", options.vsync ? "on" : "off",\n'
             '              g_display_keys.own_vsync && saved_vsync_yougame < 0 ? " (YouGame default)" : "", options.show_fps ? "on" : "off",\n'
             '              yougame::display_defaults::fps_source(saved_showfps_yougame));'),
            ('      else if (key == "showfps_yougame") saved_showfps_yougame = value == "1";',
             '      // showfps_yougame2: builds before 10-31 saved showfps_yougame 1 on every save (the old default),\n      // so a new key lets every player start from today\'s default (off; on for agent runs) once.\n      else if (key == "showfps_yougame2") saved_showfps_yougame = value == "1";'),
            ('       << (g_display_keys.yougame ? std::string("\\nshowfps_yougame ") + (options.show_fps ? "1" : "0")',
             '       << (g_display_keys.yougame ? std::string("\\nshowfps_yougame2 ") + (options.show_fps ? "1" : "0")'),
            ('           : g_display_keys.saved_showfps_yougame >= 0 ? "\\nshowfps_yougame " + std::to_string(g_display_keys.saved_showfps_yougame) : std::string())',
             '           : g_display_keys.saved_showfps_yougame >= 0 ? "\\nshowfps_yougame2 " + std::to_string(g_display_keys.saved_showfps_yougame) : std::string())'),
            ('    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;',
             '    settings_input::navigation(io, state.open || state.menu_open || practice_nav || state.fill_window);'),
            ('  const bool nav_active = state.open || state.menu_open || practice_nav;',
             '  const bool nav_active = state.open || state.menu_open || practice_nav || state.fill_window;'),
            ('  } else if (have_pad && nav_active && state.practice_pad_armed) {\n'
             '    auto& io = ImGui::GetIO();\n    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;\n',
             '  } else if (have_pad && nav_active && state.practice_pad_armed) {\n'
             '    auto& io = ImGui::GetIO();\n'),
            ('', '  // Open/close actions above also apply to this frame\'s resulting state. The\n'
             '  // neutral-release capture still prevents a closing A/B/Start reaching Melee.\n'
             '  settings_input::navigation(ImGui::GetIO(), state.open || state.menu_open || state.practice_open ||\n'
             '      practice.phase == slippi::native_practice::Phase::Failure || state.fill_window);\n'),
        ):
            self.assertEqual(s.count(new), 1)
            s = s.replace(new, old)
        # Round 17b removes the idle/version-only panel and includes update-needed
        # in the window predicate. Reverse only this bounded status block so the
        # existing whole-file pacing/observer comparison still covers everything else.
        status_block = '''    // Online menu/CSS visibility alone must not create an empty or version-only pill.
    const bool show_status = !console_status.empty() || status == yougame::update::kMessage ||
        status == yougame::friends::kWaiting || status == yougame::friends::kJoined ||
        status == yougame::friends::kFailed || status == yougame::friends::kVersion ||
        status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly;
    if (!slippi::online::is_online_match() && show_status) {
      ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 60), ImGuiCond_Always, ImVec2(0.5f, 1));
      ImGui::Begin("##yougame_friend_waiting", nullptr, passive_flags);
      ImGui::TextUnformatted(status.c_str());
'''
        build17_status_block = '''    const bool online_screen = slippi::online::online_status_visible();
    if (!slippi::online::is_online_match() && (!console_status.empty() || online_screen || status == yougame::friends::kWaiting || status == yougame::friends::kJoined ||
        status == yougame::friends::kFailed || status == yougame::friends::kVersion ||
        status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly)) {
      ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 60), ImGuiCond_Always, ImVec2(0.5f, 1));
      ImGui::Begin("##yougame_friend_waiting", nullptr, passive_flags);
      if (!console_status.empty() || status == yougame::update::kMessage || status == yougame::friends::kWaiting ||
          status == yougame::friends::kJoined || status == yougame::friends::kFailed || status == yougame::friends::kVersion ||
          status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly)
        ImGui::TextUnformatted(status.c_str());
      if (online_screen) ImGui::TextDisabled("%s", yougame::update::version_line().c_str());
'''
        self.assertEqual(s.count(status_block), 1)
        s = s.replace(status_block, build17_status_block)
        # Mixed Unranked adds only its two status lines here; reverse exact additions
        # before the existing whole-file pacing comparison.
        for addition in ('#include "yougame_blend.h"\n',
                         ' ||\n        status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly',
                         ' ||\n          status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly'):
            self.assertEqual(s.count(addition), 1)
            s = s.replace(addition, '')
        # Build 40: a friend's chat message gets its own toast under the invite toast.
        # Build 60: the invite toast starts below the corner hint. Reverse it to build 40's form first.
        for now, before in (('    // A new notice also brings the corner hint back at the picture\'s top-right; start below it.\n'
                             '    float notice_bottom = std::max(16.0f, screen_label::corner_hint_bottom() + 8.0f);\n',
                             '    float notice_bottom = 16.0f;\n'),
                            ('ImGui::SetNextWindowPos(ImVec2(screen.x - 16, notice_bottom), ImGuiCond_Always, ImVec2(1, 0));\n'
                             '      ImGui::SetNextWindowBgAlpha(0.85f);\n      ImGui::Begin("##yougame_invite_notice"',
                             'ImGui::SetNextWindowPos(ImVec2(screen.x - 16, 16), ImGuiCond_Always, ImVec2(1, 0));\n'
                             '      ImGui::SetNextWindowBgAlpha(0.85f);\n      ImGui::Begin("##yougame_invite_notice"')):
            self.assertEqual(s.count(now), 1)
            s = s.replace(now, before)
        for addition in ('    float notice_bottom = 16.0f;\n',
                         '      notice_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + 8.0f;\n      ImGui::End();\n    }\n    if (const auto message = yougame::friends::events.message(now); !message.empty()) {\n      // Under the invite toast when both show, so a chat line never hides an invite.\n      ImGui::SetNextWindowPos(ImVec2(screen.x - 16, notice_bottom), ImGuiCond_Always, ImVec2(1, 0));\n      ImGui::SetNextWindowBgAlpha(0.85f);\n      ImGui::Begin("##yougame_message_notice", nullptr, passive_flags);\n      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(380.0f, screen.x - 64));\n      ImGui::TextUnformatted(message.c_str());\n      ImGui::PopTextWrapPos();\n'):
            self.assertEqual(s.count(addition), 1)
            s = s.replace(addition, '')
        # Main round 3 changes only these settings hints in addition to the observer.
        # Console entry also reuses the Friends panel (five exact replacements below).
        # Reverse the exact approved text edits; retain the whole-file pacing comparison.
        for old, new in (
            ('yougame::parity::kMenuHint : "Off starts directly',
             'yougame::parity::kSettingsHint : "Off starts directly'),
            ('settings_toggle(yougame::stdio_bridge() ? "Show the YouGame menu reminder" : "Show the \\"Settings: F1\\" reminder",',
             'settings_toggle("Show the \\"Settings: F1\\" reminder",'),
            ('yougame::parity::kMenuHint : "F1, or Start',
             'yougame::parity::kSettingsHint : "F1, or Start'),
            ('yougame::parity::kMenuHint : "Settings: F1',
             'yougame::parity::kSettingsHint : "Settings: F1'),
            ('#include "yougame_friends.h"\n',
             '#include "yougame_friends.h"\n#include "yougame_console_entry.h"\n'),
            ('    const auto status = yougame::status_line();',
             '    const auto console_status = yougame::console::entry.status();\n    const auto status = console_status.empty() ? yougame::status_line() : console_status;'),
            ('if (!slippi::online::is_online_match() && (online_screen || status == yougame::friends::kWaiting ||',
             'if (!slippi::online::is_online_match() && (!console_status.empty() || online_screen || status == yougame::friends::kWaiting ||'),
            ('if (status == yougame::update::kMessage || status == yougame::friends::kWaiting ||',
             'if (!console_status.empty() || status == yougame::update::kMessage || status == yougame::friends::kWaiting ||'),
            ('if (status == yougame::friends::kWaiting) ImGui::TextDisabled("Z: cancel");',
             'if (status == yougame::friends::kWaiting || !console_status.empty()) ImGui::TextDisabled("Z: cancel");'),
            # 10-31: the L-cancel notice shows only when the saved assist is on, left of center.
            ('  // Ranked always shows its rule; other matchmaking modes warn when the saved assist is on.\n  if ((lcancel::automatic_enabled() || lcancel::automatic_setting_locked()) && lcancel::online_session_pending()) {',
             "  // Every matchmaking mode, Ranked included, warns only when the saved assist is on: a player who\n  // never turned it on gains nothing from the notice (10-31: it sat over Ranked setup's game boxes).\n  if (lcancel::automatic_enabled() && lcancel::online_session_pending()) {"),
            ('      ImGui::SetNextWindowPos(ImVec2((bounds.left + bounds.right) * 0.5f, bounds.top + 10), ImGuiCond_Always, ImVec2(0.5f, 0.0f));\n      ImGui::SetNextWindowBgAlpha(0.75f);\n      ImGui::Begin("LCancelModeNotice"',
             '      // Left of center: the top-center band holds Ranked setup\'s game boxes, the top-left corner the\n      // FPS counter and the top-right the YouGame menu hint.\n      ImGui::SetNextWindowPos(ImVec2(bounds.left + (bounds.right - bounds.left) * 0.28f, bounds.top + 10), ImGuiCond_Always, ImVec2(0.5f, 0.0f));\n      ImGui::SetNextWindowBgAlpha(0.75f);\n      ImGui::Begin("LCancelModeNotice"'),
        ):
            self.assertEqual(s.count(new), 1)
            s = s.replace(new, old)
        # The merged round-3 base also contains upstream/update/Friends UI changes.
        # Its observer and all other settings code must remain byte-for-byte intact.
        old = baseline('port/runtime/gx/pc_settings.cpp', 'aa8a604')
        # Build 12 removes exactly the obsolete empty-Direct friend-picker overlay.
        start = old.index('    if (yougame::friends::events.code_screen()) {')
        end = old.index('\n  }\n  ImGuiStyle launcher_saved_style;', start)
        self.assertNotIn('##yougame_code_friends', s)
        self.assertIn('state.running_vulkan', s)
        expected = old[:start] + old[end + 1:]
        # Preserve HEAD's exact status/L-cancel reversals above, then admit only
        # the immutable v0.9.83 renderer/settings byte ranges from the earlier merge.
        upstream_old = baseline('port/runtime/gx/pc_settings.cpp', 'b0d5044').splitlines(True)
        upstream_new = baseline('port/runtime/gx/pc_settings.cpp', '87ee4a4').splitlines(True)
        changes = difflib.SequenceMatcher(None, upstream_old, upstream_new, autojunk=False)
        for group in changes.get_grouped_opcodes(3):
            before = ''.join(upstream_old[group[0][1]:group[-1][2]])
            after = ''.join(upstream_new[group[0][3]:group[-1][4]])
            self.assertEqual(expected.count(before), 1, before)
            self.assertEqual(s.count(after), 1, after)
            expected = expected.replace(before, after, 1)
        self.assertEqual(s, expected)

    def test_observer_has_no_wait_flush_present_or_configuration_writes(self):
        win = source('port/runtime/gx/pacing_diagnostics_win.h')
        for forbidden in ('->Present(', 'SetMaximumFrameLatency(', 'SetFullscreenState(', 'SetWaitableTimer(',
                          'WaitForSingleObject(', 'Sleep(', 'Flush(', 'ResizeBuffers(', 'opts_.'):
            self.assertNotIn(forbidden, win)
        self.assertIn('present_seconds - minute_ < 60.0', win)
        self.assertIn('present_seconds - second_ >= 1.0', win)
        pure = source('port/runtime/gx/pacing_diagnostics.h')
        self.assertNotIn('std::vector', pure)
        self.assertNotIn('new ', pure)
        self.assertIn('DXGI_ERROR_FRAME_STATISTICS_DISJOINT', win)

    def test_test_registration_precedes_linux_return_and_build_helper_includes_it(self):
        cmake = source('CMakeLists.txt')
        self.assertLess(cmake.index('add_executable(port_pacing_diagnostics_test'), cmake.index('  return()'))
        self.assertIn("'port_pacing_diagnostics_test'", source('tools/build_linux.py'))


if __name__ == '__main__':
    unittest.main()
