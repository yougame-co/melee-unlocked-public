"""digest_compare.py: two or four games of one match compare from frame 0, and the first difference
(or missing frame) fails."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import digest_compare  # noqa: E402

HEADER = "frame,rng,scene,p0_pos_x,p1_pos_x,scene_major,match_frame,ram"


def csv_text(rows):
    """rows: (retrace, rng, p0_x, p1_x, scene_major, match_frame, ram)."""
    lines = [HEADER]
    for r in rows:
        lines.append(f"{r[0]},{r[1]},00000001,{r[2]},{r[3]},{r[4]:08X},{r[5]:08X},{r[6]}")
    return "\n".join(lines) + "\n"


def match(start_retrace, frames=5, ram="AAAA", tweak=None):
    """Menus (scene 1), three held frame-0 rows of the VS scene (2), then match frames 1..frames,
    then the menus again. tweak(match_frame, row) can change a row."""
    rows = [(start_retrace, "00000000", "0", "0", 1, 0, ram)]
    retrace = start_retrace + 1
    for held in range(3):
        rows.append((retrace, "11110000", f"F{held}", "0", 2, 0, ram))   # the scene still loading
        retrace += 1
    rows[-1] = (rows[-1][0], "12345678", "40000000", "C0000000", 2, 0, ram)   # the real frame 0
    for mf in range(1, frames + 1):
        row = (retrace, f"{mf:08X}", f"{mf:08X}", f"{mf + 100:08X}", 2, mf, ram)
        if tweak:
            row = tweak(mf, row)
        rows.append(row)
        retrace += 1
    rows.append((retrace, "00000000", "0", "0", 1, 0, ram))
    return rows


def log_text(frames, rollback_at=None, wrong=None):
    lines = ["some other log line"]
    for f in range(frames):
        x = "3F800000" if f != wrong else "40400000"
        lines.append(f"[t] online: state f {f} sum 0000{f:04X} seed 0000ABCD: p0 m 14 x {x} y 0 d 0 s 4 "
                     f"p1 m 14 x 0 y 0 d 0 s 4")
        if rollback_at == f:
            # A rolled-back frame logged first with a predicted state, then again; the last wins.
            lines.insert(-1, f"state f {f} sum DEADBEEF seed 0000ABCD: p0 m 1 x 0 y 0 d 0 s 4")
    return "\n".join(lines) + "\n"


class DigestCompareTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name, text):
        path = Path(self.dir.name) / name
        path.write_text(text)
        return str(path)

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = digest_compare.main(list(args))
        return code, out.getvalue() + err.getvalue()

    def test_identical_csvs_from_frame_0_with_other_retraces_and_ram(self):
        a = self.write("a.csv", csv_text(match(10)))
        b = self.write("b.csv", csv_text(match(400, ram="BBBB")))   # later boot, machine-local ram
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)
        self.assertIn("identical 6 frames from frame 0", out)

    def test_frame_0_is_the_last_held_row(self):
        frames = digest_compare.read_digest_csv("a", csv_text(match(10)))
        self.assertEqual(sorted(frames), list(range(6)))
        self.assertEqual(frames[0]["rng"], "12345678")

    def test_frame_0_difference_is_reported(self):
        a = self.write("a.csv", csv_text(match(10)))
        rows = match(10)
        rows[3] = (rows[3][0], "87654321") + rows[3][2:]
        b = self.write("b.csv", csv_text(rows))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1)
        self.assertIn("first difference at frame 0, field rng", out)

    def test_later_difference_names_frame_and_field(self):
        a = self.write("a.csv", csv_text(match(10)))
        b = self.write("b.csv", csv_text(match(10, tweak=lambda mf, r: r[:3] + ("BAD",) + r[4:] if mf == 3 else r)))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1)
        self.assertIn("first difference at frame 3, field p1_pos_x", out)
        self.assertIn("identical 3 frames from frame 0 before it", out)

    def test_four_games(self):
        paths = [self.write(f"{n}.csv", csv_text(match(10 * (i + 1)))) for i, n in enumerate("abcd")]
        code, out = self.run_tool(*paths)
        self.assertEqual(code, 0, out)
        paths[3] = self.write("d.csv", csv_text(match(10, tweak=lambda mf, r: r[:1] + ("FFFFFFFF",) + r[2:] if mf == 5 else r)))
        code, out = self.run_tool(*paths)
        self.assertEqual(code, 1)
        self.assertIn("frame 5", out)
        self.assertIn("d.csv", out)

    def test_three_inputs_are_refused(self):
        paths = [self.write(f"{n}.csv", csv_text(match(10))) for n in "abc"]
        code, _ = self.run_tool(*paths)
        self.assertEqual(code, 2)

    def test_different_lengths_fail_unless_common_only(self):
        a = self.write("a.csv", csv_text(match(10, frames=5)))
        b = self.write("b.csv", csv_text(match(10, frames=4)))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1)
        self.assertIn("frame 5 is missing", out)
        code, out = self.run_tool(a, b, "--common-only")
        self.assertEqual(code, 0, out)
        self.assertIn("identical 5 frames from frame 0", out)

    def test_missing_frame_0(self):
        rows = [r for r in match(10) if not (r[4] == 2 and r[5] == 0)]
        a = self.write("a.csv", csv_text(match(10)))
        b = self.write("b.csv", csv_text(rows))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1)
        self.assertIn("frame 0 is missing", out)

    def test_only_the_first_match(self):
        a = self.write("a.csv", csv_text(match(10) + match(100, tweak=lambda mf, r: r[:1] + ("X",) + r[2:])))
        b = self.write("b.csv", csv_text(match(10)))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)

    def test_trace_logs_last_line_per_frame(self):
        a = self.write("a.log", log_text(90))
        b = self.write("b.log", log_text(90, rollback_at=40))
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)
        self.assertIn("identical 90 frames from frame 0", out)
        c = self.write("c.log", log_text(90, wrong=0))
        code, out = self.run_tool(a, c)
        self.assertEqual(code, 1)
        self.assertIn("first difference at frame 0, field p0_x", out)

    def test_bad_inputs(self):
        a = self.write("a.csv", csv_text(match(10)))
        empty = self.write("e.log", "nothing here\n")
        self.assertEqual(self.run_tool(a, empty)[0], 2)
        self.assertEqual(self.run_tool(a, str(Path(self.dir.name) / "absent.csv"))[0], 2)
        log = self.write("a.log", log_text(5))
        self.assertEqual(self.run_tool(a, log)[0], 2)   # one CSV, one log


if __name__ == "__main__":
    unittest.main()
