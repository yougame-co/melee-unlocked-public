"""Build 53: the two community codes that touch the simulation are off in an online match."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'sourceport/game/shim/mu_code_features.c').read_text()
NATIVE = (ROOT / 'sourceport/game/include/mu_native.h').read_text()


class CommunityCodesOnlineTest(unittest.TestCase):
    def test_hq_fod_and_vcancel_sound_off_online_only(self):
        defines = '\n'.join(l for l in NATIVE.splitlines() if re.match(r'#define MU_DISPLAY_\w+\s+0x', l))
        start = SRC.index('#define MU_CODES_OFFLINE_ONLY')
        fn = SRC[start:SRC.index('\n}\n', SRC.index('int mu_code_feature(')) + 3]
        code = r'''
#include <stdio.h>
#include <stdlib.h>
#define OSReport(...) ((void)0)
%s
static unsigned int opts; static int online, pending, console;
unsigned int mu_display_options(void) { return opts; }
int mu_online_active(void) { return online; }
int mu_online_pending(void) { return pending; }
int mu_console_rollback_active(void) { return console; }
%s
#define CHECK(x) do { if (!(x)) { printf("FAIL %%s\n", #x); abort(); } } while (0)
int main(void) {
  opts = MU_DISPLAY_CODES_ALL;
  CHECK(mu_code_feature(MU_DISPLAY_HQ_FOD_REFLECTIONS) && mu_code_feature(MU_DISPLAY_VCANCEL_SOUND));
  for (int i = 0; i < 3; ++i) {
    online = i == 0; pending = i == 1; console = i == 2;
    CHECK(!mu_code_feature(MU_DISPLAY_HQ_FOD_REFLECTIONS) && !mu_code_feature(MU_DISPLAY_VCANCEL_SOUND));
    CHECK(mu_code_feature(MU_DISPLAY_HUD_FADE) && mu_code_feature(MU_DISPLAY_NO_GO_TEXT));
  }
  online = pending = console = 0;
  CHECK(mu_code_feature(MU_DISPLAY_HQ_FOD_REFLECTIONS));
  puts("ok");
  return 0;
}
''' % (defines, fn)
        with tempfile.TemporaryDirectory() as d:
            c = Path(d) / 't.c'
            c.write_text(code)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Werror', '-o', str(Path(d) / 't'), str(c)], check=True)
            out = subprocess.run([str(Path(d) / 't')], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == '__main__':
    unittest.main()
