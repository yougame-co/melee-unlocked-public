"""The results screen's Shift-JIS dashes and signs are bytes, not words (build 103).

The console stores them in gmresult.c as big-endian words. A little-endian build read those words'
bytes backwards: the winner's KO time drew "F-" for "-:-", stats drew "|-" for "---", and scores lost
their minus and plus signs. This compiles the native definitions from the patch and checks that each
label is the console's bytes, read through the same "%s" the screen uses.
"""
from pathlib import Path
import re
import shlex
import shutil
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'sourceport/patches/melee-native.patch'


def gmresult_section():
    patch = PATCH.read_text()
    start = patch.index('diff --git a/src/melee/gm/gmresult.c b/src/melee/gm/gmresult.c')
    return patch[start:patch.index('\ndiff --git', start + 1)]


def native_branch(section, first_label):
    """The added `#ifdef MU_NATIVE` block that defines first_label, and the console words in its #else."""
    lines = [line[1:] for line in section.splitlines() if line[:1] in ('+', ' ')]
    text = '\n'.join(lines)
    at = text.index(first_label)
    start = text.rindex('#ifdef MU_NATIVE', 0, at)
    middle = text.index('#else', at)
    end = text.index('#endif', middle)
    return text[start + len('#ifdef MU_NATIVE'):middle], text[middle + len('#else'):end]


def console_bytes(words):
    return b''.join(int(w, 16).to_bytes(4, 'big') for w in words)


def compiler():
    if os.environ.get('CC'):
        return shlex.split(os.environ['CC'])
    for name in ('cc', 'gcc', 'clang'):
        if path := shutil.which(name):
            return [path]
    raise RuntimeError('an existing C compiler is required (CC, cc, gcc, clang)')


class ResultsLabels(unittest.TestCase):
    def test_labels_are_the_consoles_bytes(self):
        section = gmresult_section()
        dash3, dash3_console = native_branch(section, 'lbl_804D3F8C')
        signs, signs_console = native_branch(section, 'lbl_804D3FA0')
        # What the console has (big-endian words), as the strings the screen draws.
        expected = {}
        console = dash3_console + signs_console
        for name in ('lbl_804D3F8C', 'lbl_804D3FA0', 'lbl_804D3FA4', 'lbl_804D3FA8'):
            # The words from the label's own definition: up to the ';' that ends it.
            definition = console[console.index(name):]
            definition = definition[:definition.index(';')]
            expected[name] = console_bytes(re.findall(r'0x[0-9A-F]{8}', definition))
        for name, raw in expected.items():
            expected[name] = raw.split(b'\0')[0]
        self.assertEqual(expected['lbl_804D3FA8'], b'\x81\x7c\x81\x46\x81\x7c')   # "-:-", the winner's KO time
        self.assertEqual(expected['lbl_804D3F8C'], b'\x81\x7c\x81\x7c\x81\x7c')   # "---"
        self.assertEqual(expected['lbl_804D3FA0'], b'\x81\x7c')                   # "-"
        self.assertEqual(expected['lbl_804D3FA4'], b'\x81\x7b')                   # "+"
        # The screen's own uses: "%s" of the label's address, and lbl_804D3FA8.text as the format.
        uses = {'lbl_804D3F8C': '&lbl_804D3F8C', 'lbl_804D3FA0': '&lbl_804D3FA0',
                'lbl_804D3FA4': '&lbl_804D3FA4', 'lbl_804D3FA8': 'lbl_804D3FA8.text'}
        body = '#include <stdio.h>\n#include <string.h>\ntypedef unsigned int u32;\n' + dash3 + signs
        body += 'static void show(const char* name, const char* s) { printf("%s", name); '
        body += 'for (; *s; ++s) printf(" %02x", (unsigned char) *s); printf("\\n"); }\n'
        body += 'int main(void) {\n'
        for name, use in uses.items():
            body += '    show("%s", (const char*) %s);\n' % (name, use)
        body += '    return 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='results-labels-') as directory:
            source, exe = Path(directory) / 'labels.c', Path(directory) / 'labels'
            source.write_text(body)
            sdk = ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []
            subprocess.run([*compiler(), '-std=gnu11', *sdk, '-Werror=incompatible-pointer-types', str(source),
                            '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        got = {}
        for line in out.splitlines():
            name, *hexes = line.split()
            got[name] = bytes(int(h, 16) for h in hexes)
        self.assertEqual(got, expected)


if __name__ == '__main__':
    unittest.main()
