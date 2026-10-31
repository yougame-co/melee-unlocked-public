"""Source regressions for the v0.9.82 renderer include conflict.

Checks upstream's software-renderer warning and the existing embedding callbacks
without starting a game or requiring a platform SDK.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class YouGame0982MergeTest(unittest.TestCase):
    def test_renderer_keeps_standard_and_guarded_mac_includes(self):
        renderer = source("port/linux/full/gx_backend_linux.cpp")
        self.assertIsNone(re.search(r"^(?:<{7}|={7}|>{7})(?: |$)", renderer, re.M))
        includes = renderer.split("namespace gx {", 1)[0]
        for header in ("string", "cctype"):
            self.assertIn("#include <" + header + ">", includes)
        self.assertIn('#ifdef __APPLE__\n#include "../../mac/embed_mac.h"\n#endif', includes)

    def test_upstream_warning_keeps_software_detection_and_automation_gate(self):
        renderer = source("port/linux/full/gx_backend_linux.cpp")
        warning = renderer.split("const std::string gl_renderer = gl_text(GL_RENDERER);", 1)[1]
        warning = warning.split('host::log("opengl: internal resolution', 1)[0]
        self.assertLess(warning.index("std::tolower(ch)"), warning.index("lower.find("))
        for name in ("llvmpipe", "softpipe", "software rasterizer", "swrast"):
            self.assertIn('lower.find("' + name + '") != std::string::npos', warning)
        self.assertIn('host::log("opengl: WARNING software rendering', warning)
        self.assertRegex(warning, r"if \(!host::options.no_gc_adapter\)[^\n]*\n\s*"
                                 r"SDL_ShowSimpleMessageBox\(SDL_MESSAGEBOX_WARNING,")

    def test_embedding_keeps_platform_specific_frame_callbacks(self):
        renderer = source("port/linux/full/gx_backend_linux.cpp")
        self.assertIn('#ifndef __APPLE__\n  if (host::window_embedded()) hooks.drawable_size =', renderer)
        self.assertIn('if (host::window_closed() || host::exit_requested()) return false;', renderer)
        self.assertIn('#ifdef __APPLE__\n    p.options.fullscreen = host::window_is_fullscreen();\n#endif', renderer)
        self.assertIn('#ifdef __APPLE__\n  hooks.after_swap = [window](uint64_t frame) {\n'
                      '    mac_embed::frame_presented(window);\n'
                      '    host::window_presented(frame);\n'
                      '  };\n#else\n'
                      '  hooks.after_swap = [](uint64_t frame) { host::window_presented(frame); };\n#endif', renderer)
        self.assertLess(renderer.index('hooks.after_swap ='),
                        renderer.index('linux_renderer::set_hooks(*renderer, std::move(hooks));'))

    def test_regression_is_registered_in_both_build_paths(self):
        for path, prefix in (("CMakeLists.txt", "port/"), ("port/CMakeLists.txt", "")):
            with self.subTest(path=path):
                cmake = source(path)
                command = ('add_test(NAME port_yougame_0982_merge COMMAND "${Python3_EXECUTABLE}" '
                           '"${CMAKE_CURRENT_SOURCE_DIR}/' + prefix + 'tests/yougame_0982_merge_test.py")')
                self.assertEqual(cmake.count(command), 1)
                if prefix:
                    self.assertLess(cmake.index(command), cmake.index('  return()'))


if __name__ == "__main__":
    unittest.main()
