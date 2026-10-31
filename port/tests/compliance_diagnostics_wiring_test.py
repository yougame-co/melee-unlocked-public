"""Shipping roster and diagnostic policy boundaries; no game or real assets."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def hook_guarded(text, needle):
    depth = []
    for line in text.splitlines():
        if line.startswith("#if"):
            depth.append("YOUGAME_AGENT_TEST_HOOK" in line)
        elif line.startswith("#else") and depth:
            depth[-1] = False
        elif line.startswith("#endif") and depth:
            depth.pop()
        if needle in line:
            return any(depth)
    return False


class ComplianceDiagnosticsTest(unittest.TestCase):
    def test_te_fake_attack_uses_session_policy(self):
        lab = source("sourceport/game/shim/mu_lab.c")
        self.assertIn('#include "mu_test_env.h"', lab)
        self.assertIn('te_fake = mu_test_getenv("MELEE_TE_FAKE_ATTACK") != NULL;', lab)
        self.assertNotRegex(lab, r'(?<!mu_test_)getenv\("MELEE_TE_FAKE_ATTACK"\)')

    def test_shipping_sources_have_no_player_identities(self):
        # Construct identifiers so this regression check itself contains no player name.
        names = ["man" + "g0", "za" + "in", "hungry" + "box", "co" + "dy", "am" + "sa", "h" + "box", "ib" + "dw"]
        pattern = re.compile(r"\b(?:" + "|".join(names) + r")\b", re.I)
        for directory in ("port", "sourceport"):
            for path in (ROOT / directory).rglob("*"):
                if path.suffix in (".c", ".cpp", ".h", ".inl", ".py", ".patch") and path.is_file():
                    # Test data only: never open environment or authentication files.
                    self.assertIsNone(pattern.search(path.read_text(encoding="utf-8-sig")), str(path.relative_to(ROOT)))

    def test_specialist_paths_use_neutral_slots(self):
        ai = source("port/runtime/hle/ghost_ai.cpp")
        self.assertIn('{"pro1", "pro2", "pro3", "pro4", "pro5"}', ai)

    def test_trace_is_guarded_and_observes_final_poll(self):
        window = source("port/runtime/host/window.cpp")
        self.assertTrue(hook_guarded(window, 'yougame::agent_test::pad_trace.poll('))
        poll = window[window.index("void input_poll(PadState out[4])"):]
        self.assertLess(poll.index("if (g_ui_capture.load())"), poll.index("yougame::agent_test::pad_trace.poll("))
        self.assertLess(poll.index("yougame::agent_test::pad_trace.poll("), poll.index("} ui{out};"))
        self.assertIn('source = "relayed slot " + std::to_string(slot + 1)', poll)
        self.assertIn('device.index == src.index', poll)
        self.assertIn('!input_script_active()', poll)
        agent = source("port/runtime/hle/yougame_agent_test.h")
        self.assertTrue(hook_guarded(agent, 'if (key == "padTrace")'))
        self.assertTrue(hook_guarded(agent, "std::string pad_trace;"))
        self.assertTrue(hook_guarded(agent, "pad_trace.arm(out.pad_trace"))
        self.assertIn("!out.pad_trace.empty()", agent)
        trace = source("port/runtime/hle/yougame_agent_pad_trace.h")
        self.assertTrue(hook_guarded(trace, "class PadTrace"))
        self.assertIn("std::chrono::steady_clock::now()", trace)
        self.assertIn("agent_paths::Mode::Append", trace)
        self.assertIn("last_[port] == pad", trace)


if __name__ == "__main__":
    unittest.main()
