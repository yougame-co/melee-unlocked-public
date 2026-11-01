#!/usr/bin/env python3
"""Reproducible system-font menu art, Pillow only; no game pixels are baked.

Round 27b fits the complete coverage -> GX IA4 -> bilinear display path to
build-26 Windows captures. Rows use Helvetica Neue Bold; title parameters are
fitted separately to the native italic Online Play tab. See the measurements
document for the search, residual errors, display anisotropy and JPEG limits.
All geometry is drawn at 16x, including fill expansion and outline dilation.
Descenders retain their natural shape: translate the whole word upward only
as far as needed to keep its complete outline inside the 30-texel image.
IA4 stores alpha in the HIGH nibble and intensity in the LOW nibble.
"""
import argparse
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = Path('/System/Library/Fonts/HelveticaNeue.ttc')
W, H, S = 176, 30, 16
ROW = dict(font=FONT, index=1, cap=19.0, weight=.125, tracking=0.0,
           outline=1.5, baseline=24.0, slant=0.0)
# Bold index 1 plus expansion matches the native tab's 8-screen-pixel stem.
# Keep the round-27 cap and slant; natural Slippi still fits before clipping.
TITLE = dict(font=FONT, index=1, cap=22.5, weight=.25, tracking=1.625,
             outline=.25, baseline=24.5, slant=.45)


def dilate(image, radius):
    radius = round(radius * S)
    return image.filter(ImageFilter.MaxFilter(2 * radius + 1)) if radius else image


def source_coverage(word, params):
    """Unclipped high-resolution glyphs; pair kerning plus explicit tracking."""
    probe = ImageFont.truetype(str(params['font']), 256, index=params['index'])
    cap = probe.getbbox('H', anchor='ls')
    font = ImageFont.truetype(str(params['font']),
                             round(params['cap'] * S * 256 / (cap[3] - cap[1])),
                             index=params['index'])
    width = math.ceil(font.getlength(word) + max(0, len(word)-1) *
                      params['tracking'] * S + 16*S)
    canvas = Image.new('L', (width, 48*S))
    draw = ImageDraw.Draw(canvas)
    x = 8*S
    for i, char in enumerate(word):
        draw.text((round(x), 32*S), char, font=font, fill=255, anchor='ls')
        if i+1 < len(word):
            x += font.getlength(word[i:i+2]) - font.getlength(word[i+1]) + params['tracking']*S
        else:
            x += font.getlength(char)
    return dilate(canvas, params['weight'])


def layout(word, title=False, params=None, height=H):
    """Return exact 1/16-texel baseline and full ink bounds, before clipping.

    One texel of exterior clearance protects row descenders from clamp-edge
    smearing. The title has only 1/4 texel clearance because its larger caps
    must share a fixed quad with the full natural p descenders.
    """
    params = dict(params or (TITLE if title else ROW))
    image = source_coverage(word, params)
    box = image.getbbox()
    radius = round(params['outline']*S)
    margin = round((.25 if title else 1.0)*S)
    baseline = min(round(params['baseline']*S), height*S-margin-(box[3]-32*S)-radius)
    top = box[1]-32*S+baseline-radius
    bottom = box[3]-32*S+baseline+radius
    if top < 0 or bottom > height*S:
        raise ValueError(f'{word}: natural glyph and outline do not fit {height} texels')
    return baseline/S, (top/S, bottom/S)


def coverage(word, title=False, *, params=None, width=W, height=H):
    params = dict(params or (TITLE if title else ROW))
    image = source_coverage(word, params)
    box = image.getbbox()
    baseline, _ = layout(word, title, params, height)
    canvas = Image.new('L', (width*S, height*S))
    x = (width*S-(box[2]-box[0]))//2-box[0]
    if box[2]-box[0]+2*round(params['outline']*S) >= width*S:
        raise ValueError(f'{word}: diagnostic word requires a wider texture')
    canvas.paste(image, (x, round((baseline-32)*S)))
    if params['slant']:
        shear = params['slant']
        canvas = canvas.transform(canvas.size, Image.Transform.AFFINE,
                                  (1, shear, -shear*baseline*S, 0, 1, 0),
                                  Image.Resampling.BICUBIC)
    outline = dilate(canvas, params['outline'])
    return tuple(p.resize((width, height), Image.Resampling.LANCZOS)
                 for p in (canvas, outline))


def linear_ia4(word, title=False, **kwargs):
    fill, outline = coverage(word, title, **kwargs)
    data = bytearray(fill.width * fill.height)
    for y in range(fill.height):
        for x in range(fill.width):
            a = outline.getpixel((x, y))
            alpha = round(a*15/255)
            intensity = min(15, round(fill.getpixel((x, y))*15/a)) if a else 0
            data[y*fill.width+x] = (alpha << 4 | intensity) if alpha else 0
    return data


def tiled(word, title=False):
    linear = linear_ia4(word, title)
    data = bytearray(W*32)  # GX IA4 8x4 tiles; rows 30/31 stay transparent.
    for y in range(H):
        for x in range(W):
            pos = ((y//4)*(W//8)+x//8)*32+y%4*8+x%8
            data[pos] = linear[y*W+x]
    return data


def header():
    lines = ['/* Generated by tools/bake_online_labels.py; do not edit. */',
             '/* Round 27b: system fonts only; natural, unsquashed descenders. */',
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


def simulated(word, title=False, selected=False, scale=(2.154, 2.39)):
    """IA4 channels are filtered separately before the material colour mix."""
    data = linear_ia4(word, title)
    alpha = Image.new('L', (W, H)); intensity = alpha.copy()
    alpha.putdata([(v >> 4)*17 for v in data])
    intensity.putdata([(v & 15)*17 for v in data])
    size = (round(W*scale[0]), round(H*scale[1]))
    alpha = alpha.resize(size, Image.Resampling.BILINEAR)
    intensity = intensity.resize(size, Image.Resampling.BILINEAR)
    fill = (0, 0, 0) if selected else (191, 146, 51)
    ring = (246, 198, 9) if selected else (6, 3, 0)
    background = (255, 202, 0) if selected else (8, 10, 23)
    ink = Image.composite(Image.new('RGB', size, fill), Image.new('RGB', size, ring), intensity)
    return Image.composite(ink, Image.new('RGB', size, background), alpha)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'sourceport/game/shim/mu_menu_label_art.h')
    parser.add_argument('--preview', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    result = header()
    if args.check:
        if args.output.read_text() != result:
            raise SystemExit('baked menu art differs; regenerate with tools/bake_online_labels.py')
    else:
        args.output.write_text(result)
    if args.preview:
        preview = Image.new('RGB', (W*3, 6*80), '#101526')
        for i, (word, title) in enumerate([('Friends', False), ('Slippi', False), ('Slippi', True)]):
            for selected in range(2):
                preview.paste(simulated(word, title, bool(selected)), (0, (i*2+selected)*80))
        preview.save(args.preview)


if __name__ == '__main__':
    main()
