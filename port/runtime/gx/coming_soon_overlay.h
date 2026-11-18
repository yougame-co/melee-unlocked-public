// YouGame's online rows while YouGame is not open yet (MELEE_YOUGAME_COMING_SOON): a rainbow
// COMING SOON! sticker drawn over the end of each of the three greyed-out rows on the online menu's
// first page, in the game's own font, popping in once the menu has settled. The game's menu says when
// that page is showing (slippi_online.cpp, the YouGame query).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct ImDrawData;

namespace coming_soon_overlay {

// The online menu's first page is on screen this frame. Game thread.
void showing();
// The first row's sticker text (a string literal that lives for the whole run). Game thread.
void first_row_text(const char* text);
// Each YouGame row's offset on screen (x, y for rows 0-2, 640x480 game pixels) from where it sits with
// the menu camera untilted; the stickers follow it. Game thread, with showing().
void set_offsets(const float offsets[6]);
// UI thread, once per presented frame: draws the stickers while the page is showing. The game
// picture is the rectangle x0,y0 to x1,y1 in the window. Badges stay below settings and dialogs.
void draw(float x0, float y0, float x1, float y1);
// The renderer, once a frame: a shader preset draws the game picture from a texture this frame, so
// the stickers go into that picture (under the preset) instead of over the window.
void set_in_picture(bool on);
// The stickers drawn this frame for the picture, for the renderer to draw into its picture texture
// (target_w x target_h pixels) before the preset runs. False: nothing to draw there.
bool picture_draw_data(ImDrawData* out, float target_w, float target_h);

}  // namespace coming_soon_overlay
