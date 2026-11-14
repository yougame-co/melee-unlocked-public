"""Native GX private objects must fit the console's embedded public storage."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def section(patch, name):
    return patch.split(f"+++ b/libs/dolphin/src/dolphin/gx/{name}", 1)[1].split("\ndiff --git ", 1)[0]


def target(section):
    # Context and added lines, without diff markers (hunks may omit unchanged code).
    return "\n".join(line[1:] for line in section.splitlines() if line.startswith((" ", "+")))


def native_block(old, new):
    return f"#ifdef MU_NATIVE\n{new}\n#else\n{old}\n#endif"


class GxObjectLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patch = (ROOT / "sourceport/patches/melee-native.patch").read_text(
            encoding="utf-8", errors="surrogateescape")
        cls.texture = target(section(cls.patch, "GXTexture.c"))

    def test_native_layout_and_console_layout(self):
        old = "\n".join(("    GXTexFmt fmt;", "    u32 tlutName;", "    u16 loadCnt;",
                         "    u8 loadFmt;", "    u8 flags;"))
        new = "\n".join(("    u8 fmt;", "    u8 tlutName;", "    u8 loadFmt;",
                         "    u8 flags;", "    u16 loadCnt;", "    u8 reserved[2];"))
        layout = re.search(r"typedef struct __GXTexObjInt_struct \{(.*?)\} __GXTexObjInt;",
                           self.texture, re.S).group(1)
        layout = re.sub(r"    /\*.*?\*/\n", "", layout, flags=re.S)
        self.assertIn(native_block(old, new), layout)
        self.assertIn("    u32 mode0;\n    u32 mode1;\n    u32 image0;\n"
                      "    u32 image3;\n    void* userData;\n", layout)
        self.assertRegex(self.texture, r"#ifdef MU_NATIVE\n/\*[^\n]*\*/\n#pragma pack\(push, 4\)\n#endif")
        self.assertIn("#ifdef MU_NATIVE\n#pragma pack(pop)\n_Static_assert", self.texture)

    def test_all_six_storage_assertions_are_native_only(self):
        pairs = {
            "GXTexture.c": [("__GXTexObjInt", "GXTexObj"), ("__GXTexRegionInt", "GXTexRegion"),
                            ("__GXTlutObjInt", "GXTlutObj"), ("__GXTlutRegionInt", "GXTlutRegion")],
            "GXLight.c": [("struct __GXLightObjInt_struct", "GXLightObj")],
            "GXFifo.c": [("struct __GXFifoObj", "GXFifoObj")],
        }
        for file, objects in pairs.items():
            code = target(section(self.patch, file))
            for internal, public in objects:
                with self.subTest(public=public):
                    check = f'_Static_assert(sizeof({internal}) <= sizeof({public}),'
                    self.assertEqual(code.count(check), 1)
                    guarded = re.search(r"#ifdef MU_NATIVE\n(?:#pragma pack\(pop\)\n)?"
                                        + re.escape(check) + r'.*?;\n#endif', code, re.S)
                    self.assertIsNotNone(guarded)

    def test_native_fifo_checks_its_own_storage(self):
        # Native builds drop GXFifo.c (sourceport/game/CMakeLists.txt) for GXFifo_native.c.
        cmake = (ROOT / "sourceport/game/CMakeLists.txt").read_text()
        self.assertIn('REMOVE_ITEM SDK_SOURCES "${SDK}/gx/GXFifo.c"', cmake)
        code = (ROOT / "sourceport/game/gxnative/GXFifo_native.c").read_text()
        check = "_Static_assert(sizeof(struct __GXFifoObj) <= sizeof(GXFifoObj),"
        self.assertEqual(code.count(check), 1)
        self.assertLess(code.index(check), code.index("void GXInitFifoBase("))

    def test_init_defines_all_public_bytes_before_field_writes(self):
        init = self.texture.split("void GXInitTexObj(", 1)[1].split("\n}", 1)[0]
        self.assertIn(native_block("    memset(t, 0, 0x20);",
                                   "    memset(obj, 0, sizeof(GXTexObj));"), init)
        first_write = re.search(r"t->\w+\s*(?:[|&]?=)", init)
        self.assertIsNotNone(first_write)
        self.assertLess(init.index("memset(obj, 0, sizeof(GXTexObj));"), first_write.start())

    def test_narrowed_fields_convert_at_every_boundary(self):
        conversions = [
            ("    t->fmt = format;", "    t->fmt = (u8) format;", 1),
            ("    t->tlutName = tlut_name;", "    t->tlutName = (u8) tlut_name;", 2),
            ("    *format = t->fmt;", "    *format = (GXTexFmt) t->fmt;", 1),
            ("    return t->fmt;", "    return (GXTexFmt) t->fmt;", 1),
            ("    return t->tlutName;", "    return (u32) t->tlutName;", 1),
            ("        tlr = (__GXTlutRegionInt*) gx->tlutRegionCallback(t->tlutName);",
             "        tlr = (__GXTlutRegionInt*) gx->tlutRegionCallback((u32) t->tlutName);", 1),
            ("            __GetImageTileCount(t->fmt, wd >> (i + 1), ht >> (i + 1),",
             "            __GetImageTileCount((GXTexFmt) t->fmt, wd >> (i + 1), ht >> (i + 1),", 1),
            ("            __GetImageTileCount(t->fmt, (u16) (wd >> (i + 1)),",
             "            __GetImageTileCount((GXTexFmt) t->fmt, (u16) (wd >> (i + 1)),", 1),
        ]
        remaining = self.texture
        for old, new, count in conversions:
            block = native_block(old, new)
            self.assertEqual(remaining.count(block), count, old)
            remaining = remaining.replace(block, "")
        # Catch a newly added use that bypasses these conversions.
        self.assertNotRegex(remaining, r"t->(?:fmt|tlutName)\b")

    def test_cmake_registers_both_checks(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn('add_test(NAME port_gx_object_layout_source COMMAND "${Python3_EXECUTABLE}" -B', cmake)
        self.assertIn('port/tests/gx_object_layout_test.py', cmake)
        self.assertIn('add_test(NAME port_gx_object_layout COMMAND port_gx_object_layout_test)', cmake)


if __name__ == "__main__":
    unittest.main()
