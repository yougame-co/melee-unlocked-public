"""Relay rumble/timing and DXGI policy wiring; no game, devices or real disc data."""
from pathlib import Path
import re
import unittest
from relay_pads_wiring_test import code, function

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def guarded(text, token):
    stack = []
    matches = []
    for line in text.splitlines():
        directive = re.match(r'\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)', line)
        if directive:
            kind, condition = directive.groups()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(kind == "ifdef" and condition.strip() == "YOUGAME_AGENT_TEST_HOOK")
            elif kind in ("else", "elif"):
                stack[-1] = False
            else:
                stack.pop()
        if token in line:
            matches.append(any(stack))
    return matches and all(matches)


class RelayOutputWiringTest(unittest.TestCase):
    def test_agent_keys_and_arming_are_hook_only(self):
        header = source("port/runtime/hle/yougame_agent_test.h")
        for token in ('"rumbleTest"', '"boxPresent"', 'bool rumble_test', 'bool box_present_compositor'):
            self.assertTrue(guarded(header, token), token)
        main = source("port/app/main.cpp")
        self.assertTrue(guarded(main, "agent_test.rumble_test"))
        self.assertTrue(guarded(main, "agent_test.box_present_compositor"))
        self.assertTrue(guarded(source("port/runtime/hle/yougame_link.cpp"), "g_rumble_pulse"))
        self.assertTrue(guarded(source("port/runtime/hle/yougame_agent_rumble.h"), "class RumblePulse"))

    def test_writer_drains_outside_mutex(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        self.assertIn("install_rumble(relay_rumble)", link)
        writer = link[link.index("void relay_rumble_writer()"):link.index("thread_local relay_pads::RouteLog")]
        self.assertRegex(writer, r'pending = g_relay_rumble.take\(command\);\s*\}')
        self.assertIn("if (pending) write_line(relay_pads::rumble_line(command));", writer)
        callback = link[link.index("void relay_rumble("):link.index("void relay_rumble_writer()")]
        self.assertNotIn("write_line(", callback)
        self.assertNotIn("WriteFile(", callback)
        self.assertIn("seat_for(", callback)
        self.assertIn("std::thread(relay_rumble_writer).detach();", link)

    def test_game_rumble_honors_enable_and_actual_source(self):
        window = source("port/runtime/host/window.cpp")
        begin = window.index("void input_rumble(int game_port")
        end = window.index("void input_rumble_local", begin)
        body = window[begin:end]
        self.assertLess(body.index("g_rumble_enabled.load()"), body.index("controller_relay::rumble(src.kind,src.index,on)"))
        self.assertIn("g_port_feeding[", body)
        self.assertIn("controller_relay::routed(src.kind, src.index, port);", window)

    def test_only_game_poll_consumes_the_frame(self):
        window = source("port/runtime/host/window.cpp")
        self.assertRegex(window, r'void input_poll\(PadState out\[4\]\)\s*\{\s*controller_relay::GamePoll relay_poll;')
        link = source("port/runtime/hle/yougame_link.cpp")
        self.assertIn("if (!host::controller_relay::game_poll) return relay_pads::controller_snapshot(g_relay_pads.snapshot());", link)
        self.assertIn("relay_pads::game_frame = g_relay_pads.game_snapshot();", link)
        state = source("port/runtime/hle/yougame_relay_pads.h")
        self.assertIn("timing_->poll(now);", state)
        self.assertNotIn("report_timing_locked", state)
        self.assertIn("const double arrived = steady_ms();", state)
        self.assertIn("g_relay_pads.timing_tick();", link)

    def test_timing_swaps_under_lock_then_formats_and_logs_after_unlock(self):
        state = code(source("port/runtime/hle/yougame_relay_pads.h"))
        tick = function(state, "timing_tick")
        self.assertRegex(tick, r'if \(!timing_->take_report\(now, completed\)\) return;\s*\}\s*char line\[384\];\s*completed.format\(')
        self.assertRegex(tick, r'pending_logs_\.push_back\(line\); return; \}\s*\}\s*log_\(line\);')
        for name in ("accept", "game_snapshot"):
            body = function(state, name)
            self.assertNotIn(".format(", body)
            self.assertNotIn("->report(", body)
            self.assertNotIn("timing_tick(", body)

    def test_dxgi_both_backends_use_policy_and_keep_queue_limits(self):
        for backend in ("d3d11", "d3d12"):
            text = source(f"port/runtime/gx/gx_{backend}.cpp")
            self.assertIn("DXGI_SWAP_EFFECT_FLIP_DISCARD", text)
            self.assertIn("box_present::choose(yougame::embedded(), opts_.vsync", text)
            self.assertIn("swapchain_->Present(present.interval, present.flags)", text)
            self.assertIn("SetMaximumFrameLatency(1)", text)
            self.assertIn("present_deadline_ > 0", text)
            self.assertIn("host::window_is_fullscreen() || fullscreen_state_.exclusive_active,", text)
            self.assertIn('#include "yougame_link.h"', text)
            self.assertIn("present_deadline_ - Stopwatch::now()", text)
        d11 = source("port/runtime/gx/gx_d3d11.cpp")
        self.assertIn("flip_model_ = sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;", d11)
        d12 = source("port/runtime/gx/gx_d3d12.cpp")
        self.assertIn("if (!skip_present_) wait_frame_latency();", d12)
        self.assertIn("fg_applied_ <= 0 && opts_.reflex_mode <= 0", d12)

    def test_registrations_precede_linux_return(self):
        cmake = source("CMakeLists.txt")
        cutoff = cmake.index("  return()")
        for name in ("port_yougame_mode_policy", "port_opponent_display", "port_yougame_embed_focus_wiring"):
            self.assertLess(cmake.index("add_test(NAME " + name + " "), cutoff)
            self.assertEqual(cmake.count("add_test(NAME " + name + " "), 1)


if __name__ == "__main__":
    unittest.main()
