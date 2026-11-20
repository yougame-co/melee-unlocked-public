// The arena room in the black bars beside the game picture, on the online character select
// (hle/slippi_arena.h). Controller only: a tap of Z opens the room menu, which then takes the
// buttons (X give up my spot / get in line / just watch, Y rules, Z add a friend's code, A ask or
// answer, R doubles, B close) while the game keeps X, Y, L and R for costumes and skins and the
// C-stick for its camera. Z is the room's in a room (the room runs the search there, so the game's
// "Z to cancel" has nothing to do) and while no search is running; a plain Direct search keeps it.
//
// Looks: the combined one (corners on top, the line and the stands under them) is the default;
// Card, Split, Corners and Ring are the others. Every look slides in from the sides when the
// character select comes back, and can be folded into small tabs at the edges that open on hover.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

struct ImFont;
struct ImDrawData;

namespace arena_panel {

enum class Look : uint8_t { Combined = 0, Card = 1, Split = 2, Corners = 3, Ring = 4 };
constexpr int kLookCount = 5;

struct Frame {
  float display_w = 0, display_h = 0;
  float game_x0 = 0, game_y0 = 0, game_x1 = 0, game_y1 = 0;   // the game picture in the window
  uint16_t buttons = 0;          // GameCube buttons of the player's controller
  int8_t stick_x = 0, stick_y = 0, sub_x = 0, sub_y = 0;
  double time = 0;               // seconds
  bool on_online_menus = false;  // the online character select (or waiting there)
};

// The panel's lettering, loaded with the overlay's fonts (any may be null: ImGui's current font).
struct Fonts { ImFont* title = nullptr; ImFont* black = nullptr; ImFont* bold = nullptr; ImFont* body = nullptr; };
void set_fonts(const Fonts& fonts);

// The two choices a player makes about the panel (also on the room's Rules page), for Settings:
// its look and whether it folds into tabs until the hand comes near. Saved at once.
int look();
const char* look_name(int look);
void set_look(int look);
bool tabs();
void set_tabs(bool on);
// Out of a room, the Arena card in the right bar: 0 shows for ten seconds at the session's first Direct
// character select and again when the hand or mouse comes near that side, 1 always, 2 never.
constexpr int kButtonModeCount = 3;
int button_mode();
const char* button_mode_name(int mode);
void set_button_mode(int mode);

// UI thread, once per overlay frame, inside ImGui. Draws nothing outside the online menus.
void frame(const Frame& f);
// Out of a room, the Arena card under the hands: the renderer says before the overlay frame whether
// this game frame has the native select's mark (gx Frame::under_hand_command) and it will draw the
// card there. Without it the card is drawn over the window as everything else.
// efb_width: the game picture's width in the renderer's internal pixels (640 times the internal
// scale), 0 when unknown. Below about 0.6 of the picture's window width the card stays over the
// window: drawn into so few pixels its small text broke up (internal resolution 1x on a large window).
void set_under_hand(bool on, float efb_width = 0.0f);
// The card's ImGui lists for the renderer to draw into its picture at the mark, built in the
// current ImGui frame. The game picture lands on (x, y, w, h) of a target of target_w x target_h
// pixels. False when the card was not drawn into the picture this frame.
bool under_hand_draw_data(ImDrawData* out, float x, float y, float w, float h, float target_w, float target_h);
// True while the room menu has the controller (the game must not see those presses).
bool wants_capture();
// GameCube buttons the game must not see even while the menu is shut: Z, which opens the room
// (in a room the room runs the search, so the game's "Z to cancel" has nothing to do there).
uint16_t withheld_buttons();

}  // namespace arena_panel
