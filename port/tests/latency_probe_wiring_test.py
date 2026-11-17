"""Latency probe (MELEE_TRACE_LATENCY=1 with --script): one line per injected press, wired only
behind the existing trace switch, with no new environment variable."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


class LatencyProbeWiringTest(unittest.TestCase):
    def test_line_format(self):
        backend = source("port/runtime/gx/threaded_backend.cpp")
        self.assertIn('"latency probe: press %u injected %.3f pad read %.2f present return %.2f presented %s"', backend)
        self.assertIn('char presented[32] = "n/a";', backend)
        self.assertIn("if (latency_trace && current.tick.probe > probe_logged) {", backend)

    def test_off_without_the_trace(self):
        window = source("port/runtime/host/window.cpp")
        self.assertIn("if (latency_trace_enabled()) note_script_presses(out);", window)
        probe = window[window.index("static void note_script_presses"):window.index("static void script_pads(")]
        self.assertNotIn("getenv", probe)
        # The per-tick reset only rewrites the trace's own fields; the probe's stay 0 unless a press set them.
        host = source("port/runtime/host/host.cpp")
        self.assertNotIn("g_tick_timing = {", host)
        self.assertIn("g_tick_timing.pad = 0; g_tick_timing.pad_age = -1;", host)

    def test_presented_only_from_display_timing(self):
        core = source("port/runtime/gx/gx_core.h")
        self.assertIn("virtual int probe_presented(uint64_t, double*) { return -1; }", core)
        vulkan = source("port/runtime/gx/gx_vulkan.cpp")
        self.assertIn("uint64_t probe_present_id() override { return display_timing_on_ ? present_id_ : 0; }", vulkan)
        self.assertNotIn("probe_presented", source("port/runtime/gx/gx_d3d11.cpp"))
        self.assertNotIn("probe_presented", source("port/runtime/gx/gx_d3d12.cpp"))

    def test_test_env_still_cut_in_yougame(self):
        self.assertIn("return yougame_session ? nullptr : std::getenv(name);", source("port/runtime/host/test_env.h"))


if __name__ == "__main__":
    unittest.main()
