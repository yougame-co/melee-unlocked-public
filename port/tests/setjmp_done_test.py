"""Every game setjmp buffer is cleared before its function returns (Friends join-in-progress).

__builtin_setjmp stores a host stack address in the buffer, and the buffers live in game state. Each
is only used within one call, so a clear before every return keeps a stale, per-process address out
of the state the guest compares. The decomp has exactly three setjmp sites; a new one fails here.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "sourceport/extern/melee/src"
# file, function, buffer
SITES = [
    ("melee/gr/granime.c", "grAnime_801C8318", "grAnime_8049EE40"),
    ("sysdolphin/baselib/hsd_3B34.c", "hsd_803B51C8_inline", "hsd_804D2648.buf"),
    ("sysdolphin/baselib/hsd_3B5C.c", "hsd_803B6BE4_inline", "state.work->jmp"),
]
DIRECTIVE = re.compile(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")


def section(patch, path):
    return patch.split(f"+++ b/{path}", 1)[1].split("\ndiff --git ", 1)[0]


def strip_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group()), text, flags=re.S)


def blank(line):
    return " " * len(line)


def first_branches(text):
    """Only the first branch of each #if/#ifdef (alternatives repeat an opening brace), with every
    removed line blanked so offsets match the source."""
    kept, skip = [], []
    for line in text.split("\n"):
        directive = DIRECTIVE.match(line)
        if directive is None:
            kept.append(blank(line) if any(skip) else line)
            continue
        kind = directive.group(1)
        if kind.startswith("if"):
            skip.append(False)
        elif kind == "endif":
            skip.pop()
        else:
            skip[-1] = True
        kept.append(blank(line))
    return "\n".join(kept)


def all_branches(text):
    return "\n".join(blank(line) if DIRECTIVE.match(line) else line for line in text.split("\n"))


def body(text, function):
    """Offsets of the function's definition body, braces included."""
    match = re.search(rf"^[^\n;]*\b{re.escape(function)}\([^;{{]*\)\s*{{", text, re.M)
    if match is None:
        raise AssertionError(f"no definition of {function}")
    start = match.end() - 1
    depth = 0
    for i in range(start, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return start, i + 1
    raise AssertionError(f"unbalanced braces in {function}")


class SetjmpDoneTest(unittest.TestCase):
    def test_patch_defines_and_uses_the_clear(self):
        patch = (ROOT / "sourceport/patches/melee-native.patch").read_text(encoding="utf-8", errors="surrogateescape")
        header = section(patch, "src/Runtime/Gecko_setjmp.h")
        self.assertIn("+#define SETJMP_DONE(env) __builtin_memset(&(env), 0, sizeof(env))\n", header)
        self.assertIn("+#define SETJMP_DONE(env) ((void) 0)\n", header)
        for path, function, buffer in SITES:
            self.assertGreaterEqual(section(patch, f"src/{path}").count(f"+    SETJMP_DONE({buffer});\n"), 1, path)

    @unittest.skipUnless((SRC / "Runtime/Gecko_setjmp.h").is_file(),
                         "prepared decomp absent: Runtime/Gecko_setjmp.h is missing")
    def test_every_return_clears_the_buffer(self):
        setjmp_h = SRC / "Runtime/Gecko_setjmp.h"
        self.assertIn("__builtin_setjmp", setjmp_h.read_text(), "prepare the sources first: tools/prepare_native_sources.py")
        for path, function, buffer in SITES:
            text = strip_comments((SRC / path).read_text(encoding="utf-8", errors="surrogateescape"))
            start, end = body(first_branches(text), function)
            code = all_branches(text)[start:end]  # returns in every #if branch count
            self.assertIn(f"setjmp({buffer})", code, f"{function} no longer sets up {buffer}")
            statements = [s.strip() for s in re.split(r"[;{}]", code) if s.strip()]
            returns = [i for i, s in enumerate(statements) if re.search(r"\breturn\b", s)]
            self.assertTrue(returns, function)
            for i in returns:
                self.assertTrue(statements[i].startswith("return"), f"{function}: '{statements[i]}' hides a return")
                # A call in the return expression could still longjmp into the cleared buffer.
                self.assertIsNone(re.search(r"\w\s*\(", statements[i][6:]), f"{function}: '{statements[i]}' calls out")
                self.assertEqual(statements[i - 1], f"SETJMP_DONE({buffer})", f"{function}: '{statements[i]}' without the clear")
            # Cleared only on the way out: each clear is followed by a return.
            for i, s in enumerate(statements):
                if s.startswith("SETJMP_DONE("):
                    after = statements[i + 1] if i + 1 < len(statements) else ""
                    self.assertTrue(after.startswith("return"), f"{function}: clear not at an exit")

    @unittest.skipUnless((SRC / "Runtime/Gecko_setjmp.h").is_file(),
                         "prepared decomp absent: Runtime/Gecko_setjmp.h is missing")
    def test_no_other_setjmp_site(self):
        sites = set()
        for source in sorted(SRC.rglob("*.[ch]")):
            if source.parts[len(SRC.parts)] == "Runtime":
                continue
            text = strip_comments(source.read_text(encoding="utf-8", errors="surrogateescape"))
            for match in re.finditer(r"\b_*setjmp\s*\(\s*([^)]*)\)", text):
                sites.add((source.relative_to(SRC).as_posix(), match.group(1).strip()))
        self.assertEqual(sites, {(path, buffer) for path, _, buffer in SITES})


if __name__ == "__main__":
    unittest.main()
