"""Regression checks for the v0.9.81 CPU/Linux conflicts and retained lead patch.

No game process, platform SDK, external files or network access required.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class YouGame0981MergeTest(unittest.TestCase):
    def test_cpu_baseline_default_is_independent_of_mac_check(self):
        main = source("port/app/main.cpp")
        self.assertIsNone(re.search(r"^(?:<{7}|={7}|>{7})(?: |$)", main, re.M))
        self.assertIn('#ifdef __APPLE__\n#include "../mac/cpu_check.h"\n#endif\n'
                      '#ifndef MELEE_CPU_BASELINE_NAME\n#define MELEE_CPU_BASELINE_NAME "AVX2"\n#endif', main)
        self.assertIn('#ifdef __APPLE__\n  return mac_host::cpu_ok();\n#endif', main)
        self.assertIn('if (std::strcmp(MELEE_CPU_BASELINE_NAME, "AVX2") == 0 && !cpu_has_avx2())', main)

    def test_linux_compat_names_keep_c_helper_out_of_cpp_preinclude(self):
        cmake = source("port/linux/full/CMakeLists.txt")
        self.assertIsNone(re.search(r"^(?:<{7}|={7}|>{7})(?: |$)", cmake, re.M))
        for declaration in ('set(MELEE_GAME_FILE melee_game.so)',
                            'set(MELEE_SOURCE_NAME melee_source)',
                            'set(MELEE_GAME_FILE melee_game_compat.so)',
                            'set(MELEE_SOURCE_NAME melee_source_compat)'):
            self.assertIn(declaration, cmake)
        host = cmake.split('add_executable(melee_source ', 1)[1]
        self.assertIn('../embed/yougame_embed_x11.c', host)
        self.assertIn('MELEE_GAME_LIBRARY_NAME="${MELEE_GAME_FILE}")', host)
        self.assertIn('set_target_properties(melee_source PROPERTIES OUTPUT_NAME ${MELEE_SOURCE_NAME})', host)
        self.assertIn('target_compile_options(melee_source PRIVATE '
                      '"$<$<COMPILE_LANGUAGE:CXX>:${MELEE_LINUX_COMMON_FLAGS}>")', host)
        self.assertNotIn('target_compile_options(melee_source PRIVATE ${MELEE_LINUX_COMMON_FLAGS})', host)
        self.assertIn('target_compile_features(melee_source PRIVATE c_std_99)', host)
        self.assertEqual(host.count('$<TARGET_FILE_DIR:melee_source>/${MELEE_GAME_FILE}'), 2)

    def test_lead_practice_overrides_wait_for_session_and_respect_saved_config(self):
        cpu = source("port/runtime/hle/practice_cpu.cpp")
        self.assertIn('#include "test_env.h"', cpu)
        initial = cpu.split('MuPcConfig g_config = [] {', 1)[1].split('}();', 1)[0]
        self.assertNotIn('getenv', initial)
        apply = cpu.split('void apply_test_env_locked() {', 1)[1].split('std::atomic<uint32_t>', 1)[0]
        self.assertLess(apply.index('if (g_test_env_done) return;'), apply.index('host::test_env::getenv'))
        self.assertLess(apply.index('g_test_env_done = true;'), apply.index('host::test_env::getenv'))
        for name in ('MELEE_TEST_PRACTICE_CPU', 'MELEE_TEST_PRACTICE_GHOST', 'MELEE_PC_ANY_MODE'):
            self.assertIn('host::test_env::getenv("' + name + '")', cpu)
            self.assertNotIn('std::getenv("' + name + '")', cpu)
        self.assertIn('std::lock_guard<std::mutex> lk(g_mutex); apply_test_env_locked(); return g_config;', cpu)
        setter = cpu.split('void set_config(const MuPcConfig& c) {', 1)[1].split('uint32_t generation()', 1)[0]
        self.assertLess(setter.index('g_test_env_done = true;'), setter.index('std::memcmp'))

    def test_lead_unreleased_ghost_and_native_diagnostic_gates_remain(self):
        ghosts = source("port/runtime/hle/practice_ghosts.h")
        self.assertIn('kReleased[] = {false, false, false, false, false};', ghosts)
        cpu = source("port/runtime/hle/practice_cpu.cpp")
        select = cpu.split('bool ghost(int index, MuPcConfig& c) {', 1)[1].split('return true;', 1)[0]
        self.assertLess(select.index('if (!practice_ghosts::kReleased[index]) return false;'),
                        select.index('mu_pc_defaults(&c);'))
        menu = source("port/runtime/gx/practice_menu.cpp")
        self.assertIn('#include "test_env.h"', menu)
        self.assertIn('if (!practice_ghosts::kReleased[c.preset & 0x7F]) return "";', menu)
        self.assertIn('host::test_env::getenv("MELEE_TEST_PRACTICE_MENU")', menu)
        native = source("sourceport/game/shim/mu_practice_cpu.c")
        self.assertIn('#include "mu_test_env.h"', native)
        for name in ('MELEE_PC_ANY_MODE', 'MELEE_TEST_AI_P1', 'MELEE_TEST_PC_PORT'):
            self.assertIn('mu_test_getenv("' + name + '")', native)
            self.assertNotRegex(native, r'(?<!mu_test_)\bgetenv\("' + name + r'"\)')

    def test_regression_is_registered_in_both_build_paths(self):
        for path, prefix in (("CMakeLists.txt", "port/"), ("port/CMakeLists.txt", "")):
            with self.subTest(path=path):
                cmake = source(path)
                command = ('add_test(NAME port_yougame_0981_merge COMMAND "${Python3_EXECUTABLE}" '
                           '"${CMAKE_CURRENT_SOURCE_DIR}/' + prefix + 'tests/yougame_0981_merge_test.py")')
                self.assertEqual(cmake.count(command), 1)
                if not prefix:
                    continue
                self.assertLess(cmake.index(command), cmake.index('  return()'))


if __name__ == "__main__":
    unittest.main()
