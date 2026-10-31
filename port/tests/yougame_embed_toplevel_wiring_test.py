"""Windows toplevel contract wiring; reads source and never launches the game."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def block(text, marker):
    start = text.index("{", text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


class ToplevelWiringTest(unittest.TestCase):
    def setUp(self):
        self.window = source("port/runtime/host/window.cpp")
        self.link = source("port/runtime/hle/yougame_link.cpp")

    def test_env_is_taken_before_reader_and_ignored_off_windows(self):
        start = block(self.link, "void start()")
        flags = start[start.index('#ifdef _WIN32'):start.index('#endif', start.index('#ifdef _WIN32'))]
        self.assertIn('enabled(g_embedded, take("YOUGAME_EMBED_TOPLEVEL"))', flags)
        self.assertIn('app_pid(take("YOUGAME_APP_PID"))', flags)
        self.assertLess(start.index('take("YOUGAME_APP_PID")'), start.index('std::thread(read_stdin)'))
        self.assertIn('bool g_embed_toplevel = false;', self.link)

    def test_caps_follow_successful_window_creation(self):
        create = block(self.window, 'void* window_create(')
        self.assertLess(create.index('if (!g_hwnd) die('), create.index('yougame::embed_window_ready(reinterpret_cast<uintptr_t>(g_hwnd));'))
        self.assertIn('if (!g_embed_toplevel) parity::signals.start(true,', self.link)
        ready = block(self.link, 'void embed_window_ready(uintptr_t hwnd)')
        self.assertIn('if (g_embed_toplevel)', ready)
        self.assertIn('parity::signals.start(g_stdio,', ready)
        self.assertIn('}, true);', ready)
        self.assertIn('if (hwnd) parity::signals.emit(embed_toplevel_policy::hwnd_line(hwnd).c_str());', ready)

    def test_styles_are_chosen_at_creation_and_owner_is_external(self):
        create = block(self.window, 'void* window_create(')
        self.assertIn('styles(yougame::embed_toplevel(), flip, Backend::D3D12,', create)
        self.assertIn('CreateWindowExW(style.exstyle,', create)
        self.assertIn('title, style.style,', create)
        adopted = block(create, 'const auto boxAdopted = []')
        self.assertIn('if (yougame::embed_toplevel()) return GetWindow(g_hwnd, GW_OWNER) != nullptr;', adopted)
        for call in ('SetParent(', 'SetForegroundWindow(', 'SetActiveWindow(', 'AttachThreadInput('):
            self.assertIsNone(re.search(r"\b" + re.escape(call), self.window), call)
        # The only parent-focus operations remain in the legacy child case, after a toplevel return.
        for marker in ('case kBoxFocus:', 'case kBoxShow: {'):
            case = self.window[self.window.index(marker):]
            top = block(case, 'if (yougame::embed_toplevel())')
            self.assertNotIn('GetParent(', top)
            self.assertIn('return 0;', top)
            self.assertGreater(case.index('SetFocus(GetParent(h))'), case.index(top) + len(top))

    def test_screen_rects_bypass_child_clipping_and_zorder(self):
        apply = block(self.window, 'void apply_toplevel_geometry(')
        self.assertIn('first(w), second(w), first(l), second(l)', apply)
        self.assertIn('SWP_NOACTIVATE | SWP_NOZORDER', apply)
        self.assertNotIn('SetWindowRgn', apply)
        handler = block(self.window, 'yougame::set_box_handler([window]')
        top = block(handler, 'if (yougame::embed_toplevel())')
        self.assertIn('rect(line, true, box)', top)
        self.assertIn('PostMessageW(window, kBoxScreenRect,', top)
        self.assertIn('pack(box.x, box.y)', top)
        self.assertNotIn('0xFFFF', top)
        self.assertIn('if (line.rfind("view ", 0) == 0) return;', top)
        self.assertIn('if (yougame::embed_toplevel()) return;', block(self.window, 'void raise_embed('))

    def test_dpi_and_fullscreen_are_app_owned(self):
        start = block(self.link, 'void start()')
        top = block(start, 'if (g_embed_toplevel)')
        failed = block(top, 'if (!SetProcessDpiAwarenessContext(')
        self.assertIn('g_toplevel_dpi_error = GetLastError();', failed)
        self.assertIn('g_toplevel_dpi_failed = true;', failed)
        self.assertNotIn('host::log(', top)
        logging = block(self.link, 'void logging_ready()')
        self.assertIn('std::call_once(once,', logging)
        self.assertIn('if (g_embed_toplevel && g_toplevel_dpi_failed)', logging)
        self.assertIn('embed: toplevel DPI awareness failed err=%lu', logging)
        create = block(self.window, 'void* window_create(')
        self.assertNotIn('requires per-monitor-v2 DPI awareness', create)
        dpi = self.window[self.window.index('case WM_DPICHANGED:'):]
        top = block(dpi, 'if (yougame::embed_toplevel())')
        self.assertIn('g_on_dpi(HIWORD(w))', top)
        self.assertIn('return 0;', top)
        self.assertNotIn('SetWindowPos', top)
        for path in ('port/app/main.cpp', 'port/runtime/gx/threaded_backend.cpp'):
            text = source(path)
            self.assertRegex(text, r'#ifdef _WIN32\s*(?://[^\n]*\n\s*)?if \(yougame::embed_toplevel\(\)\) \w+\.exclusive_fullscreen = false;')
        fullscreen = block(self.window, 'void window_set_fullscreen(')
        self.assertIn('if (yougame::embedded()) return;', fullscreen)

    def test_flip_surface_is_declared_in_both_game_window_paths(self):
        for path, arg in (('port/app/main.cpp', '!hidden'), ('port/runtime/gx/threaded_backend.cpp', 'visible')):
            self.assertIn(f'{arg}\n#ifdef _WIN32\n', source(path))
            self.assertIn(', true\n#endif', source(path))
        d11 = source('port/runtime/gx/gx_d3d11.cpp')
        fallback = d11[d11.index('if (FAILED(swapchain_hr))'):]
        self.assertLess(fallback.index('if (yougame::embed_toplevel()) require(swapchain_hr,'),
                        fallback.index('sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD'))
        for backend in ('d3d11', 'd3d12'):
            text = source(f'port/runtime/gx/gx_{backend}.cpp')
            self.assertIn('synced, yougame::embed_toplevel());', text)
            fullscreen = block(text, '::apply_fullscreen_mode()')
            self.assertIn('if (yougame::embed_toplevel()) opts_.exclusive_fullscreen = false;', fullscreen)

    def test_focus_return_grants_only_the_pid_then_emits_then_logs(self):
        focus = block(self.window, 'void return_toplevel_focus(HWND h)')
        self.assertIn('if (!yougame::embed_toplevel_policy::emit_focus_return(GetForegroundWindow() == h, g_has_focus.load())) return;', focus)
        self.assertLess(focus.index('emit_focus_return('), focus.index('toplevel_app_pid(h)'))
        self.assertIn('if (pid && !AllowSetForegroundWindow(pid))', focus)
        failed = block(focus, 'if (pid && !AllowSetForegroundWindow(pid))')
        self.assertIn('const DWORD error = GetLastError();', failed)
        self.assertIn('if (!failure_logged)', failed)
        self.assertIn('embed: toplevel AllowSetForegroundWindow failed err=%lu', failed)
        self.assertLess(focus.index('AllowSetForegroundWindow(pid)'), focus.index('signals.emit('))
        self.assertIn('signals.emit(yougame::embed_toplevel_policy::kFocusReturn)', focus)
        self.assertIn('if (g_toplevel_focus.changed())', focus)
        self.assertIn('focus_return_message(pid)', focus)
        show = block(self.window[self.window.index('case kBoxShow: {'):], 'if (yougame::embed_toplevel())')
        self.assertLess(show.index('if (!w) return_toplevel_focus(h);'), show.index('ShowWindow('))
        focus = block(self.window[self.window.index('case kBoxFocus:'):], 'if (yougame::embed_toplevel())')
        self.assertIn('if (w && IsWindowVisible(h))', focus)
        self.assertIn('SetFocus(h);', focus)
        self.assertIn('else if (!w) return_toplevel_focus(h);', focus)
        self.assertIn('if (w && !foreground_logged)', focus)
        self.assertIn('embed: toplevel foreground=%d', focus)
        self.assertLess(focus.index('SetFocus(h);'), focus.index('embed: toplevel foreground=%d'))

    def test_toplevel_uses_foreground_focus_and_prefers_owner_pid(self):
        focused = block(self.window, 'bool window_focused(HWND h)')
        self.assertIn('if (yougame::embed_toplevel()) return GetForegroundWindow() == h;', focused)
        self.assertIn('return yougame::embedded() ? g_has_focus.load() : GetForegroundWindow() == h;', focused)
        pid = block(self.window, 'uint32_t toplevel_app_pid(HWND h)')
        self.assertIn('GetWindowThreadProcessId(GetWindow(h, GW_OWNER), &owner_pid)', pid)
        self.assertIn('preferred_app_pid(owner_pid, env_pid)', pid)
        self.assertIn('owner_pid && env_pid && owner_pid != env_pid && !mismatch_logged', pid)
        self.assertIn('embed: toplevel owner pid %lu differs from YOUGAME_APP_PID %u', pid)

    def test_toplevel_waits_for_placement_and_falls_back_after_timeout(self):
        apply = block(self.window, 'void apply_toplevel_geometry(')
        self.assertLess(apply.index('if (!SetWindowPos('), apply.index('g_toplevel_rect = true;'))
        self.assertLess(apply.index('g_toplevel_rect = true;'), apply.index('ShowWindow('))
        self.assertIn('if (g_toplevel_shown && !IsWindowVisible(h))', apply)
        show = block(self.window[self.window.index('case kBoxShow: {'):], 'if (yougame::embed_toplevel())')
        self.assertIn('if (!w || g_toplevel_rect || g_toplevel_fallback) ShowWindow(', show)
        create = block(self.window, 'void* window_create(')
        init = block(create, 'if (yougame::embed_toplevel())')
        self.assertIn('g_toplevel_shown = visible;', init)
        self.assertIn('g_toplevel_fallback = false;', init)
        self.assertIn('const ULONGLONG until = GetTickCount64() + 30000;', create)
        self.assertIn('while ((!boxAdopted() || (yougame::embed_toplevel() && !g_toplevel_rect)) && GetTickCount64() < until)', create)
        boxed = block(create, 'if (boxed)')
        adoption = block(boxed[boxed.index('Sleep(5);'):], 'if (yougame::embed_toplevel())')
        self.assertIn('if (boxAdopted()) toplevel_app_pid(g_hwnd);', adoption)
        fallback = block(adoption, 'if (!boxAdopted() || !g_toplevel_rect)')
        self.assertIn('g_toplevel_fallback = true;', fallback)
        self.assertIn('GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW', fallback)
        self.assertIn('SWP_FRAMECHANGED', fallback)
        self.assertIn('embed: toplevel never adopted; shown as a normal window', fallback)
        self.assertLess(fallback.index('SetWindowLongPtrW('), fallback.index('ShowWindow('))
        # No unconditional show remains after the adoption/placement gate.
        tail = create[create.index('RegisterRawInputDevices(rid, 2, sizeof rid[0]);'):]
        self.assertNotIn('ShowWindow(', block(tail, 'if (yougame::embed_toplevel())'))
        screen_rect = self.window[self.window.index('case kBoxScreenRect:'):self.window.index('case kBoxRect:')]
        self.assertIn('if (yougame::embed_toplevel()) apply_toplevel_geometry(h, w, l);', screen_rect)

    def test_registration_precedes_linux_return(self):
        cmake = source('CMakeLists.txt')
        self.assertLess(cmake.index('include(port/tests/embed_toplevel.cmake)'), cmake.index('  return()'))
        self.assertIn("'port_yougame_embed_toplevel_test'", source('tools/build_linux.py'))


if __name__ == '__main__':
    unittest.main()
