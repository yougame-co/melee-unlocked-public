#!/usr/bin/env python3
"""Bake custom menu words, offline only (Python 3 + Pillow, macOS DIN Alternate).

Build-19 reference: Ranked 202x45 px, Unranked 261x47 px, stems 8-9 px,
outline 2-3 px, row pitch ~120 px. At 45 px cap height DIN predicts word
advances 201.4/261.2; Arial Bold 226.7/289.5, Rounded 226.0/292.3, Narrow
185.9/237.2. DIN therefore matches both words without horizontal fitting.
Rows: 18 px cap, baseline 24, native ~2.5 screen px/texel, .85 px outline.
DIN's unusually long p descenders are fitted to four pixels below that baseline
so their outline fits inside the 30 px native texture without moving the caps.
Title: same face with a 14 degree forward slant, 18 px cap, baseline 21;
the slant adds ~10 screen px to DIN's 285 px Online Play advance at cap 43,
matching the reference tab's ~296 px width. Padding clears the tab border.
IA4 stores alpha in the HIGH nibble, intensity in the LOW nibble. A white
fill and black outline retain the game's animated black/gold material colours.
No copyrighted game pixels or system font files are copied into the output.
"""
import argparse
import math
import json
import statistics
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = Path('/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf')
W, H, S = 176, 30, 16


def measure(path):
    """Screen-space measurements; JPEG edges/perspective are not source texels."""
    image = Image.open(path).convert('RGB')
    def ink(box, predicate):
        points = {(x, y) for y in range(box[1], box[3]) for x in range(box[0], box[2])
                  if predicate(*image.getpixel((x, y)))}
        bounds = (min(x for x, y in points), min(y for x, y in points),
                  max(x for x, y in points), max(y for x, y in points))
        return points, bounds
    ranked, rb = ink((590, 310, 810, 372), lambda r, g, b: r < 70 and g < 65 and b < 45)
    _, ub = ink((485, 430, 775, 490), lambda r, g, b: r > 125 and g > 95 and b < 100 and r > 1.1*g)
    _, hb = ink((697, 100, 997, 145), lambda r, g, b: min(r, g, b) > 100 and max(r, g, b)-min(r, g, b) < 70)
    remaining, glyphs = set(ranked), []
    while remaining:
        component = {remaining.pop()}
        pending = list(component)
        while pending:
            x, y = pending.pop()
            for neighbour in ((x-1, y), (x+1, y), (x, y-1), (x, y+1)):
                if neighbour in remaining:
                    remaining.remove(neighbour)
                    component.add(neighbour)
                    pending.append(neighbour)
        if len(component) > 20:
            glyphs.append((min(x for x, y in component), max(x for x, y in component)))
    glyphs.sort()
    stems = []
    for y in range(339, 351):
        row = sorted(x for x in range(595, 615) if (x, y) in ranked)
        if row:
            run = 1
            while run < len(row) and row[run] == row[0] + run:
                run += 1
            stems.append(run)
    fits = {}
    for name in ('DIN Alternate Bold', 'Arial Bold', 'Arial Rounded Bold', 'Arial Narrow Bold'):
        font = ImageFont.truetype(str(FONT.parent / (name + '.ttf')), 180)
        cap = font.getbbox('H')
        fits[name] = [round(font.getlength(word)*45/(cap[3]-cap[1]), 1) for word in ('Ranked', 'Unranked')]
    pitch = (ub[1]+ub[3]-rb[1]-rb[3])/2
    print(json.dumps(dict(ranked_bounds=rb, unranked_bounds=ub, title_bounds=hb,
                          ranked_glyph_x=glyphs, advances=[b[0]-a[0] for a, b in zip(glyphs, glyphs[1:])],
                          median_R_stem=statistics.median(stems), row_pitch=pitch,
                          cap_per_pitch=round((rb[3]-rb[1]+1)/pitch, 3), candidate_advances=fits), indent=2))


def coverage(word, title=False):
    probe = ImageFont.truetype(str(FONT), 256)
    cap = probe.getbbox('H', anchor='ls')
    font = ImageFont.truetype(str(FONT), round(18 * S * 256 / (cap[3] - cap[1])))
    canvas = Image.new('L', (W * S, (H + 8) * S))
    draw = ImageDraw.Draw(canvas)
    left, _, right, _ = draw.textbbox((0, 0), word, font=font, anchor='ls')
    x = (W * S - (right - left)) // 2 - left
    draw.text((x, (21 if title else 24) * S), word, font=font, fill=255, anchor='ls')
    if not title:
        descent = font.getbbox(word, anchor='ls')[3]
        if descent > 4 * S:
            lower = canvas.crop((0, 24*S, W*S, 24*S+descent))
            lower = lower.resize((W*S, 4*S), Image.Resampling.LANCZOS)
            canvas.paste(0, (0, 24*S, W*S, canvas.height))
            canvas.paste(lower, (0, 24*S))
    if title:
        shear = math.tan(math.radians(14))
        canvas = canvas.transform(canvas.size, Image.Transform.AFFINE,
                                  (1, shear, -shear * 21 * S, 0, 1, 0),
                                  Image.Resampling.BICUBIC)
    # Dilate at 16x resolution, then downsample once: smooth fill AND outline.
    outline = canvas.filter(ImageFilter.MaxFilter(2 * round(.85 * S) + 1))
    return tuple(p.crop((0, 0, W*S, H*S)).resize((W, H), Image.Resampling.LANCZOS)
                 for p in (canvas, outline))


def tiled(word, title=False):
    fill, outline = coverage(word, title)
    data = bytearray(W * 32)  # GX IA4 8x4 tiles, including two zero padding rows
    for y in range(H):
        for x in range(W):
            a = outline.getpixel((x, y))
            alpha = round(a * 15 / 255)
            intensity = min(15, round(fill.getpixel((x, y)) * 15 / a)) if a else 0
            pos = ((y // 4) * (W // 8) + x // 8) * 32 + y % 4 * 8 + x % 8
            data[pos] = (alpha << 4 | intensity) if alpha else 0
    return data


def header():
    lines = ['/* Generated by tools/bake_online_labels.py; do not edit. */',
             '#ifndef MU_MENU_LABEL_ART_H', '#define MU_MENU_LABEL_ART_H',
             'enum { MU_LABEL_W = 176, MU_LABEL_H = 30, MU_LABEL_BYTES = 176 * 32 };']
    for symbol, word, title in [('friends', 'Friends', False), ('slippi', 'Slippi', False),
                                ('slippi_title', 'Slippi', True)]:
        lines.append('static const unsigned char mu_baked_' + symbol +
                     '[MU_LABEL_BYTES] __attribute__((aligned(32))) = {')
        data = tiled(word, title)
        lines.extend('    ' + ','.join(f'0x{v:02x}' for v in data[i:i+32]) + ','
                     for i in range(0, len(data), 32))
        lines.append('};')
    return '\n'.join(lines + ['#endif', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'sourceport/game/shim/mu_menu_label_art.h')
    parser.add_argument('--preview', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--measure', type=Path, help='measure the supplied build-19 native frame')
    args = parser.parse_args()
    if args.measure:
        measure(args.measure)
    result = header()
    if args.check:
        if args.output.read_text() != result:
            raise SystemExit('baked menu art differs; regenerate with tools/bake_online_labels.py')
    else:
        args.output.write_text(result)
    if args.preview:
        preview = Image.new('RGB', (W * 4, H * 4 * 6), '#101526')
        for i, (word, title) in enumerate([('Friends', False), ('Slippi', False), ('Slippi', True)]):
            fill, outline = coverage(word, title)
            for selected in range(2):
                row = Image.new('RGB', (W, H), '#ffce00' if selected else '#101526')
                row.paste('#000000', (0, 0), outline)
                row.paste('#000000' if selected else '#be933e', (0, 0), fill)
                preview.paste(row.resize((W * 4, H * 4)), (0, (i * 2 + selected) * H * 4))
        preview.save(args.preview)


if __name__ == '__main__':
    main()
