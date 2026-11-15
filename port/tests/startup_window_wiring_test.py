"""Build 103: the window and caps come before the slow startup work, and startup lines carry timing.

11-15 (a test PC, 0.6 GB free): sysgen, the retail-disc check, the card and the disc prefetch took about
90 s before the window existed, past the YouGame app's 30 s caps wait for a first run, and the app
then kept the game in child mode with a frozen picture.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class StartupWindowWiringTest(unittest.TestCase):
    def setUp(self):
        self.main = source("port/app/main.cpp")
        self.body = self.main.split("static int melee_main(int argc, char** argv) {", 1)[1]

    def test_render_thread_window_is_made_right_after_the_disc_opens(self):
        body = self.body
        disc = body.index('if (!host::disc_open(o.iso)) { std::fprintf(stderr, "cannot open ISO')
        early = body.index("#ifdef _WIN32\n  if (threaded && !headless) make_window();\n#endif")
        self.assertLess(disc, early)
        # Before every slow phase.
        for slow in ("host::prepare_sys_assets(sysgen_user_dir, sysgen_data_dir);",
                     "host::cosmetics::scan_look_packs();\n    const std::string look",
                     "source_port::mods::identify_vanilla(",
                     "host::disc_prefetch_wait();",
                     "gx::init(backend.get());"):
            self.assertLess(early, body.index(slow), slow)
        # The title and the embedded exclusive-fullscreen rule come before the window, as before.
        self.assertLess(body.index("host::window_title_base() = base;"), early)
        self.assertLess(body.index("if (yougame::embedded()) gfx.exclusive_fullscreen = false;"), early)
        # One creation path, made once: the old site only makes it when the early one did not.
        self.assertEqual(body.count("gx::create_threaded_backend(gfx, !hidden)"), 1)
        self.assertEqual(body.count("std::unique_ptr<gx::Backend> backend;"), 1)
        late = body.index("  if (!window_made) make_window();")
        self.assertLess(body.index("#ifdef __APPLE__\n  // Embedded launch belongs to YouGame"), late)
        self.assertLess(late, body.index("host::disc_prefetch_wait();"))
        self.assertIn("auto make_window = [&] {\n    window_made = true;", body)

    def test_the_window_block_touches_no_game_state(self):
        block = self.body.split("auto make_window = [&] {", 1)[1].split("\n  };\n", 1)[0]
        for forbidden in ("ppc::", "host::ram", "source_port::", "gecko::", "slippi::", "o.card_dir",
                          "set_replay", "mods::", "rng", "hle::"):
            self.assertNotIn(forbidden, block, forbidden)

    def test_startup_lines_keep_their_wording_and_end_in_timing(self):
        main = self.main
        self.assertIn('host::log("gameplay: verified retail disc; vanilla-only memory card %s%s", o.card_dir.c_str(), host::startup_mark().c_str());', main)
        self.assertIn('host::log("gameplay: %s%s", verification.c_str(), host::startup_mark().c_str());', main)
        self.assertIn('"settings: %s, frame cap %s, sub-frame %s, internal resolution %s, upscaler %d, vsync %s, %s%s",', main)
        self.assertIn('host::log("boot: entering __start at %08X%s", 0x8000522Cu, host::startup_mark().c_str());', main)
        self.assertIn('host::log("startup: starting the game%s", host::startup_mark().c_str());', main)
        self.assertIn('host::log("startup: window and renderer ready%s", host::startup_mark().c_str());', main)
        host = source("port/runtime/host/host.cpp")
        self.assertIn('log("disc: image prefetched into the file cache (%llu MB in %.1f s)%s"', host)
        self.assertIn("GB more the system can hand out%s\\n\"", host)
        self.assertIn('log("startup: first frame presented%s", startup_mark().c_str());', host)
        self.assertIn("GetProcessTimes(GetCurrentProcess()", host)
        sysgen = source("port/runtime/host/sysgen_boot.cpp")
        self.assertIn('host::log("%s%s", assets.status_message().c_str(), host::startup_mark().c_str());', sysgen)
        link = source("port/runtime/hle/yougame_link.cpp")
        ready = link.split("void embed_window_ready(uintptr_t hwnd) {", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(ready.index("parity::signals.start(g_stdio"), ready.index('"startup: toplevel caps sent%s"'))

    def test_first_frame_is_marked_where_ready_is_sent(self):
        for path, line in (("port/runtime/gx/gx_d3d12.cpp", "      yougame::parity::signals.presented(!test_only && SUCCEEDED(present_result) && frame.test_fill < 0);\n"
                                                             "      if (!test_only && SUCCEEDED(present_result) && frame.test_fill < 0) host::startup_first_frame();"),
                           ("port/runtime/gx/gx_d3d11.cpp", "    yougame::parity::signals.presented(!test_only && SUCCEEDED(present_result) && frame.test_fill < 0);\r\n"
                                                            "    if (!test_only && SUCCEEDED(present_result) && frame.test_fill < 0) host::startup_first_frame();"),
                           ("port/runtime/gx/gx_vulkan.cpp", "        yougame::parity::signals.presented(real_frame);\n"
                                                             "        if (real_frame) host::startup_first_frame();")):
            text = (ROOT / path).read_bytes().decode("utf-8-sig")
            self.assertIn(line, text, path)

    def test_full_build_label_reaches_the_log_header(self):
        label = source("YOUGAME_BUILD").strip()
        version = source("VERSION").strip()
        self.assertRegex(label, r"^[A-Za-z0-9._+-]{1,64}$")
        self.assertTrue(label.startswith(version + "-"), label)
        cmake = source("port/cmake/yougame_build.cmake")
        self.assertIn('"${PROJECT_SOURCE_DIR}/YOUGAME_BUILD"', cmake)
        port = source("port/CMakeLists.txt")
        self.assertIn("include(cmake/yougame_build.cmake)", port)
        self.assertEqual(len(re.findall(r'MELEE_YOUGAME_BUILD="\$\{MELEE_YOUGAME_BUILD\}"', port)), 2)
        host = source("port/runtime/host/host.cpp")
        self.assertIn("startup::log_header(MELEE_PORT_VERSION, name ? name + 1 : exe, MELEE_YOUGAME_BUILD)", host)
        self.assertNotIn('"Melee Unlocked %s, %s\\n", MELEE_PORT_VERSION', host)


if __name__ == "__main__":
    unittest.main()
