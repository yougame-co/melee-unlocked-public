/* Native menu label textures, like rogue/mu_rogue_label.c, but composed from
 * SIS's built-in glyphs instead of hard-coded slices of disc label images.
 * Kept independent of HSD so tests can supply a wholly synthetic font. */
#ifndef MU_MENU_LABEL_H
#define MU_MENU_LABEL_H

enum { MU_LABEL_W = 176, MU_LABEL_H = 30, MU_LABEL_BYTES = 176 * 32 };

static unsigned mu_label_i4(const unsigned char* glyph, int x, int y)
{
    int pixel = ((y / 8) * 4 + x / 8) * 64 + (y % 8) * 8 + x % 8;
    return (glyph[pixel / 2] >> (x % 2 ? 0 : 4)) & 15;
}

/* The ASCII map contains big-endian SIS opcodes, the atlas 32x32 tiled I4
 * cells, and metrics the left/right margins SIS uses for proportional text.
 * No disc image or generated game pixels are stored in this implementation. */
static int mu_friends_label_render(unsigned char* out, const unsigned char* atlas,
                                   const unsigned char* ascii, const unsigned char* metrics)
{
    static const char text[] = "Friends";
    unsigned codes[7];
    int advances[7], i, x, y, width = 0, top = 32, bottom = -1;
    unsigned char plane[MU_LABEL_W * MU_LABEL_H] = {0};
    if (!out || !atlas || !ascii || !metrics) return 0;
    for (i = 0; i < 7; ++i) {
        unsigned index = ((unsigned char) text[i] - 0x20) * 2;
        unsigned code = (ascii[index] << 8) | ascii[index + 1];
        if (code < 0x2000 || code >= 0x2000 + 287) return 0;
        codes[i] = code - 0x2000;
        advances[i] = 34 - metrics[codes[i] * 2] - metrics[codes[i] * 2 + 1];
        if (advances[i] <= 0 || advances[i] > 32) return 0;
        width += advances[i];
        for (y = 0; y < 32; ++y)
            for (x = 0; x < 32; ++x)
                if (mu_label_i4(atlas + codes[i] * 512, x, y)) {
                    if (y < top) top = y;
                    if (y > bottom) bottom = y;
                }
    }
    if (bottom < top || width > MU_LABEL_W - 12) return 0;
    /* Fill the native label's height, with the modest slant of its option art.
     * The material supplies the normal gold / selected black colour. */
    width = (MU_LABEL_W - width - 6) / 2;
    for (i = 0; i < 7; ++i) {
        const unsigned char* glyph = atlas + codes[i] * 512;
        int left = metrics[codes[i] * 2] - 1;
        for (y = 2; y < MU_LABEL_H - 2; ++y) {
            int sy = top + (y - 2) * (bottom - top + 1) / (MU_LABEL_H - 4);
            for (x = 0; x < 32; ++x) {
                int dx = width + x - left + (MU_LABEL_H - 1 - y) / 5;
                unsigned v = mu_label_i4(glyph, x, sy);
                if (dx >= 0 && dx < MU_LABEL_W && v > plane[y * MU_LABEL_W + dx])
                    plane[y * MU_LABEL_W + dx] = (unsigned char) v;
            }
        }
        width += advances[i];
    }
    /* GX tiled IA4, including the two padding rows of the final 8x4 tile. */
    for (i = 0; i < MU_LABEL_BYTES; ++i) out[i] = 0;
    for (y = 0; y < MU_LABEL_H; ++y)
        for (x = 0; x < MU_LABEL_W; ++x) {
            int pos = ((y / 4) * (MU_LABEL_W / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8;
            unsigned v = plane[y * MU_LABEL_W + x];
            out[pos] = v ? (unsigned char) ((v << 4) | 15) : 0;
        }
    return 1;
}

#endif
