#!/usr/bin/env python3
"""Doubles partner portraits (Find, Invite): simple shapes baked to GX RGBA8, stdlib only.

The Doubles select (sourceport/game/include/mu_friends4_css.inc) draws one of these in the partner door's
portrait slot while that seat is empty and the partner tab is on Find or Invite (11-11 report: "I want the
character portrait to change to toggle between the different options"). P2 and CPU need no art: they are
Melee's own Player 2 and CPU doors.

Find: a fighter's silhouette with a big "?" (anyone the search finds). Invite: two silhouettes and a green
"+" badge (a friend). Both sit on a dark backdrop, square: the part of the portrait above the name plate (14.2 x 13.9 units).
No game pixels, fonts or outside art: every texel comes from the shapes below, 4x4 supersampled.

  python3 -I tools/bake_doubles_partner_art.py            # write the header
  python3 -I tools/bake_doubles_partner_art.py --check    # fail if the header is stale
  python3 -I tools/bake_doubles_partner_art.py --preview DIR   # also write find.png / invite.png to look at
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
W, H, SS = 48, 48, 4
OUTPUT = ROOT / 'sourceport/game/include/mu_doubles_partner_art.h'
NAMES = ('find', 'invite')

BACK_TOP = (0x1E, 0x27, 0x3E)
BACK_BOTTOM = (0x07, 0x09, 0x10)
FIGURE = (0x4A, 0x5C, 0x82)
FIGURE_BACK = (0x2E, 0x3A, 0x55)
MARK = (0xFF, 0xFF, 0xFF)
OUTLINE = (0x10, 0x14, 0x22)
BADGE = (0x33, 0xC0, 0x4A)


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ellipse(cx, cy, rx, ry, bottom):
    return lambda x, y: y <= bottom and ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


def ring(cx, cy, r, width, start, end):
    """An arc of a ring, angles in degrees, 0 = right, clockwise on screen (y down), start to end."""
    def inside(x, y):
        d = math.hypot(x - cx, y - cy)
        if abs(d - r) > width / 2:
            return False
        a = math.degrees(math.atan2(y - cy, x - cx)) % 360
        return (a - start) % 360 <= (end - start) % 360
    return inside


def box(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def capsule(ax, ay, bx, by, r):
    def inside(x, y):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
        return (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= r * r
    return inside


def union(*shapes):
    return lambda x, y: any(s(x, y) for s in shapes)


def figure(cx, head_y, scale, bottom=H):
    """A fighter's silhouette: a head and rounded shoulders."""
    return union(circle(cx, head_y, 8.0 * scale), ellipse(cx, head_y + 29.0 * scale, 16.0 * scale, 17.0 * scale, bottom))


def layers(name):
    """Back to front: (shape, rgb)."""
    if name == 'find':
        def question(grow):
            return union(ring(24.0, 14.0, 7.5, 3.6 + 2 * grow, 165.0, 80.0),   # the hook, from the left over the top
                         capsule(25.3, 21.4, 24.0, 25.5, 1.8 + grow),          # down to the stem
                         circle(24.0, 31.6, 2.3 + grow))                       # the dot
        # A dark edge keeps the mark clear where it crosses the figure.
        return [(figure(24.0, 21.0, 1.0), FIGURE), (question(1.3), OUTLINE), (question(0.0), MARK)]
    if name == 'invite':
        plus = union(box(34.0, 9.9, 44.0, 12.1), box(37.9, 6.0, 40.1, 16.0))
        return [(figure(17.0, 20.0, 0.8), FIGURE_BACK), (figure(28.0, 24.0, 0.92), FIGURE),
                (circle(39.0, 11.0, 8.9), OUTLINE), (circle(39.0, 11.0, 7.6), BADGE), (plus, MARK)]
    raise ValueError(name)


def render(name):
    """RGBA bytes, row-major, top row first."""
    out = bytearray()
    shapes = layers(name)
    for py in range(H):
        t = py / (H - 1)
        back = tuple(round(a + (b - a) * t) for a, b in zip(BACK_TOP, BACK_BOTTOM))
        for px in range(W):
            acc = [0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    x, y = px + (sx + 0.5) / SS, py + (sy + 0.5) / SS
                    rgb = back
                    for inside, colour in shapes:
                        if inside(x, y):
                            rgb = colour
                    for k in range(3):
                        acc[k] += rgb[k]
            out.extend(round(v / (SS * SS)) for v in acc)
            out.append(255)
    return bytes(out)


def gx_rgba8(rgba):
    """GX RGBA8: 4x4 tiles, each 16 AR pairs then 16 GB pairs."""
    tiled = bytearray()
    for y in range(0, H, 4):
        for x in range(0, W, 4):
            pixels = [rgba[((y + j) * W + x + i) * 4:((y + j) * W + x + i) * 4 + 4] for j in range(4) for i in range(4)]
            tiled.extend(v for p in pixels for v in (p[3], p[0]))
            tiled.extend(v for p in pixels for v in (p[1], p[2]))
    return bytes(tiled)


def png(rgba):
    raw = b''.join(b'\0' + rgba[y * W * 4:(y + 1) * W * 4] for y in range(H))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def generate():
    lines = ['/* Generated by tools/bake_doubles_partner_art.py: the Doubles partner door\'s Find and Invite portraits,',
             ' * drawn from simple shapes (no game pixels). GX RGBA8, 4x4 tiles. Do not edit. */',
             '#ifndef MU_DOUBLES_PARTNER_ART_H', '#define MU_DOUBLES_PARTNER_ART_H',
             f'#define MU_DPA_W {W}', f'#define MU_DPA_H {H}',
             '#ifdef __cplusplus', '#define MU_DPA_ALIGN alignas(32)', '#else',
             '#define MU_DPA_ALIGN _Alignas(32)', '#endif']
    for name in NAMES:
        tiled = gx_rgba8(render(name))
        lines.append(f'MU_DPA_ALIGN static const unsigned char mu_dpa_{name}[{len(tiled)}] = {{')
        lines.extend('    ' + ','.join(str(v) for v in tiled[i:i + 32]) + ',' for i in range(0, len(tiled), 32))
        lines.append('};')
    lines.extend(['#undef MU_DPA_ALIGN', '#endif', ''])
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, default=OUTPUT)
    p.add_argument('--check', action='store_true')
    p.add_argument('--preview', type=Path, help='also write find.png and invite.png here')
    args = p.parse_args()
    header = generate()
    if args.check:
        if not args.output.exists() or args.output.read_text() != header:
            raise SystemExit(f'Stale Doubles partner art: {args.output} (run tools/bake_doubles_partner_art.py)')
    elif not args.output.exists() or args.output.read_text() != header:
        args.output.write_text(header)
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        for name in NAMES:
            (args.preview / f'{name}.png').write_bytes(png(render(name)))
    print(f'Doubles partner art: {W}x{H} RGBA8, {len(NAMES)} portraits, {"verified" if args.check else "baked"}')


if __name__ == '__main__':
    main()
