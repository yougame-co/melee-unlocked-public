"""Relay input gates shared by Source and Static hosts (no app or game launch)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def code(text):
    # Ignore comments and strings so mentions of an API cannot satisfy the check.
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                  " ", text, flags=re.S)


def block(text, start):
    depth = 1
    for end in range(start, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if depth == 0:
            return text[start:end]
    raise AssertionError("Unclosed block")


def function(text, name):
    match = re.search(r'\b' + name + r'\([^;{}]*\)\s*\{', text)
    if not match:
        raise AssertionError("Missing function: " + name)
    return block(text, match.end())


class RelayPadsWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.window = code(source("port/runtime/host/window.cpp"))

    def test_state_asks_provider_then_relay_before_native_work(self):
        state = function(self.window, "xinput_state")
        # The app's provider (yougame_controller_relay.h) first: the raw relay seats read every
        # relayed pad as an Xbox pad, so with them first a PlayStation or Switch pad was read twice.
        provider = re.match(r'\s*if\s*\(controller_relay::active\(\)\)\s*\{', state)
        self.assertIsNotNone(provider)
        sample = block(state, provider.end())
        self.assertNotIn("relay_pads", sample)
        self.assertNotIn("xinput_ask(", sample)
        self.assertIn("return false;", sample)
        self.assertIn("return true;", sample)
        after = state[provider.end() + len(sample) + 1:]
        self.assertRegex(after, r'^\s*if\s*\(yougame::relay_pads::active\(\)\)\s*'
                                r'return yougame::relay_pads::read\(idx, state\);')

    def test_native_reads_only_inside_poll(self):
        ask = function(self.window, "xinput_ask")
        gate = re.search(r'yougame::relay_pads::poll\(idx, state, \[&\]\s*\{', ask)
        self.assertIsNotNone(gate)
        native = block(ask, gate.end())
        self.assertRegex(native, r'return XInputGetState\(\(DWORD\)idx, state\) == ERROR_SUCCESS;')
        # With the provider installed nothing is asked: neither Windows nor the raw relay seats.
        self.assertRegex(ask, r'^\s*if\s*\(controller_relay::active\(\)\)\s*return ERROR_DEVICE_NOT_CONNECTED;')
        self.assertEqual(len(re.findall(r'\bXInputGetState\s*\(', native)), 1)
        self.assertEqual(len(re.findall(r'\bXInputGetState\s*\(', self.window)), 1)

    def test_probe_thread_checks_relay_before_probing(self):
        probe = function(self.window, "xinput_probe_start")
        self.assertRegex(probe, r'^\s*if\s*\(yougame::relay_pads::active\(\)\)\s*return;')
        thread = re.search(r'std::thread\(\[\]\s*\{', probe)
        self.assertIsNotNone(thread)
        worker = block(probe, thread.end())
        self.assertRegex(worker, r'for\s*\(int idx = 0; idx < 4; \+\+idx\)\s*\{\s*'
                                r'if\s*\(yougame::relay_pads::active\(\)\)\s*return;')
        self.assertLess(worker.index("yougame::relay_pads::active()"), worker.index("xinput_ask("))

    def test_rumble_gates_all_native_writes(self):
        rumble = function(self.window, "xinput_rumble")
        self.assertRegex(rumble, r'if\s*\(yougame::relay_pads::active\(\)\)\s*\{\s*'
                                r'yougame::relay_pads::rumble\(\[\]\s*\{\}\);\s*return;\s*\}')
        self.assertLess(rumble.index("yougame::relay_pads::rumble("), rumble.index("std::thread("))
        writer = function(self.window, "xinput_rumble_writer")
        gate = re.search(r'yougame::relay_pads::rumble\(\[&\]\s*\{', writer)
        self.assertIsNotNone(gate)
        native = block(writer, gate.end())
        self.assertRegex(native, r'XInputSetState\(\(DWORD\)index, &v\);')
        self.assertEqual(len(re.findall(r'\bXInputSetState\s*\(', native)), 1)
        self.assertEqual(len(re.findall(r'\bXInputSetState\s*\(', self.window)), 1)

    def test_source_and_static_link_window_runtime(self):
        cmake = re.sub(r'#[^\n]*', '', source("port/CMakeLists.txt"))
        # window.cpp enters the shared runtime through the host source glob.
        self.assertRegex(cmake, r'file\(GLOB RUNTIME_SOURCES\b[^)]*runtime/host/\*\.cpp')
        self.assertIn('add_library(runtime STATIC ${RUNTIME_SOURCES}', cmake)
        removals = re.findall(r'list\(REMOVE_ITEM RUNTIME_SOURCES\b([^)]*)\)', cmake)
        for removed in removals:
            self.assertNotIn("window.cpp", removed)
            self.assertNotIn("runtime/host/*.cpp", removed)
        for target in ("melee_source", "melee_port"):
            self.assertRegex(cmake, r'add_executable\(' + target + r'\s')
            self.assertRegex(cmake, r'target_link_libraries\(' + target + r' PRIVATE runtime\b')

    def test_link_installs_provider_before_the_relay_latches(self):
        link = function(code(source("port/runtime/hle/yougame_link.cpp")), "handle_app_line")
        install = link.index("host::controller_relay::install(relayed_controllers);")
        self.assertLess(link.index("if (relay_pads::parse(line, frame, reason))"), install)
        self.assertLess(install, link.index("g_relay_pads.accept(line, now_ms());"))
        self.assertEqual(link.count("controller_relay::install("), 1)

    def test_hid_stays_native_until_the_app_sends_hid(self):
        poll = function(self.window, "relay_hid_poll")
        self.assertRegex(poll, r'^\s*return relay_owns_hid\(\) \?')
        self.assertIn("yougame::relay_pads::owns_hid()", function(self.window, "relay_owns_hid"))
        name = function(code(source("port/runtime/host/hid_pad.cpp")), "hidpad_name")
        self.assertRegex(name, r'if \(controller_relay::sample\(CaptureDevice::HidPad,slot,s\)\) return s\.name;\s*\}')

    def test_gameplay_and_preview_use_relay_fallback_facts(self):
        for name, snapshot in (("read_devices", "debug"), ("input_debug_snapshot", "snapshot")):
            body = function(self.window, name)
            self.assertIn(f"controller_relay::fallback_facts(input_device_facts({snapshot}))", body)
            self.assertIn("effective_port_source(g_port_sources, facts, port)", body)

    def test_eof_resets_relay_before_exit(self):
        link = function(code(source("port/runtime/hle/yougame_link.cpp")), "read_stdin")
        self.assertIn("g_relay_pads.reset_on_eof();", link)
        self.assertLess(link.index("g_relay_pads.reset_on_eof();"), link.index("host::request_exit(0);"))


if __name__ == "__main__":
    unittest.main()
