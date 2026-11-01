"""Check that the shipped source PNGs produce the pinned GX badge textures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('rasterize_rank_badges', ROOT/'tools/rasterize_rank_badges.py')
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)


def png(raw, width=48):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, 48, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class BadgeSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header, manifest = bake.generate()
        cls.manifest = json.loads(manifest)

    def test_generated_asset_matches_pngs(self):
        self.assertEqual((ROOT/'sourceport/game/shim/assets/mu_rank_badges.h').read_text(), self.header)
        self.assertEqual(json.loads((ROOT/'tools/rank_badges/checksums.json').read_text()), self.manifest)

    def test_header_identical_to_726b4b7(self):
        self.assertEqual(hashlib.sha256(self.header.encode()).hexdigest(),
                         'e75651fb6c65885c2d0ba5aaf7bae933d6dc306fdbaaff007347775a7d64ba7f')

    def test_dimensions_and_sha256_per_tier(self):
        self.assertEqual((self.manifest['width'], self.manifest['height']), (48, 48))
        textures = self.manifest['textures']
        self.assertEqual(len(textures), 16)
        self.assertEqual({p.name for p in bake.PNG_DIR.glob('*.png')}, {bake.png_name(n) for n in bake.NAMES})
        for name, entry in textures.items():
            data = (bake.PNG_DIR/entry['png']).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['png_sha256'])
            self.assertEqual(hashlib.sha256(bake.decode_png(data)).hexdigest(), entry['rgba_sha256'])
            self.assertEqual(hashlib.sha256(bake.gx_tile(bake.decode_png(data))).hexdigest(), entry['gx_sha256'])
        self.assertEqual(len({textures[f'{t}_placed']['rgba_sha256'] for t in bake.TIERS}), 6)

    def test_both_gx_planes_and_pixel_order(self):
        rgba = bytes(v % 256 for v in range(48*48*4))
        gx = bake.gx_tile(rgba)
        decoded = bytearray(len(rgba))
        for y in range(48):
            for x in range(48):
                b = (y//4*12+x//4)*64; p = (y % 4*4+x % 4)*2
                decoded[(y*48+x)*4:(y*48+x)*4+4] = bytes((gx[b+p+1], gx[b+32+p], gx[b+32+p+1], gx[b+p]))
        self.assertEqual(bytes(decoded), rgba)

    def test_all_png_filters(self):
        rgba = bytes((x*31 + x//192*17) % 256 for x in range(48*48*4))
        for mode in range(5):
            raw = bytearray()
            for y in range(48):
                raw.append(mode)
                for x in range(192):
                    i = y*192+x
                    left = rgba[i-4] if x >= 4 else 0
                    up = rgba[i-192] if y else 0
                    corner = rgba[i-196] if y and x >= 4 else 0
                    predictors = [0, left, up, (left+up)//2]
                    p = left+up-corner
                    predictors.append(min((left, up, corner), key=lambda v: abs(p-v)))
                    raw.append((rgba[i]-predictors[mode]) % 256)
            with self.subTest(filter=mode):
                self.assertEqual(bake.decode_png(png(raw)), rgba)

    def test_reject_invalid_pngs(self):
        raw = (b'\0'+bytes(192))*48
        valid = png(raw)
        damaged = bytearray(valid)
        damaged[29] ^= 1
        for data in (b'', valid[:-1], bytes(damaged), png(raw, width=47),
                     png(raw[:-1]), png((b'\5'+bytes(192))*48)):
            with self.subTest(length=len(data)):
                with self.assertRaises(ValueError):
                    bake.decode_png(data)

    def test_png_pixel_change_reaches_texture(self):
        original = (b'\0'+bytes(192))*48
        changed = bytearray(original)
        changed[1:5] = bytes((10, 20, 30, 255))
        self.assertNotEqual(bake.gx_tile(bake.decode_png(png(original))),
                            bake.gx_tile(bake.decode_png(png(changed))))


if __name__ == '__main__': unittest.main()
