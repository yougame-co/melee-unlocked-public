"""Streamline's own start (slInit) faulting must not end the game, and its fault note and logs go where an
AppContainer-started game may write (122.34: an access violation inside sl.interposer during slInit closed the
game one second after launch)."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = (ROOT / "port/runtime/gx/gx_streamline.cpp").read_text(encoding="utf-8-sig")


class StreamlineInitFaultTest(unittest.TestCase):
    def test_sl_init_runs_under_structured_exception_handling(self):
        start = SRC.index("static sl::Result seh_sl_init(")
        guard = SRC[start:SRC.index("bool init(", start)]
        self.assertIn("__try", guard)
        self.assertIn("return slInit(*pref, sl::kSDKVersion);", guard)
        self.assertIn("__except (EXCEPTION_EXECUTE_HANDLER)", guard)
        self.assertIn("driver_call([&] { return seh_sl_init(&pref, &init_faulted); })", SRC)
        self.assertNotIn("driver_call([&] { return slInit(pref, sl::kSDKVersion); })", SRC)

    def test_a_fault_starts_without_streamline_and_notes_it(self):
        body = SRC[SRC.index("if (init_faulted) {"):]
        body = body[:body.index("}")]
        self.assertIn("write_fault_note(", body)
        self.assertIn("return false;", body)

    def test_note_and_logs_go_to_a_writable_folder(self):
        self.assertIn('_wgetenv(L"YOUGAME_DATA")', SRC)
        self.assertIn('g_fault_note = writable + L"/streamline-faulted.txt";', SRC)
        self.assertIn('logs = writable + L"\\\\streamline-logs";', SRC)
        self.assertIn("pref.pathToLogsAndData = logs_ok ? logs.c_str() : nullptr;", SRC)
        self.assertNotIn('logs = exe_dir + L"\\\\streamline-logs"', SRC)


if __name__ == "__main__":
    unittest.main()
