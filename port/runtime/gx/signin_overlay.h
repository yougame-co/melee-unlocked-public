// The "Sign in to YouGame" panel over the game (yougame_signin.h, local-channel-contract.md section 8):
// Melee started on its own reached a YouGame app that is signed out, and the player picked a YouGame
// row. Shows the app's short code, the address to type it at, a QR of the app's link and the time
// left, with "Sign in on this PC" (the browser sign-in) and Back. Mouse, keyboard and controller: the
// buttons are ImGui buttons on the settings panel's routed pad navigation (D-pad or stick to move, A to
// press, B or Esc for Back), and the game gets no input while the panel is up.
// The same panel says "Play online with YouGame" (yougame_signin.h Reach) when a YouGame row is picked
// with no YouGame connected: why it can't start, with Open YouGame or Get YouGame, and Back.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace signin_overlay {

// The panel is up (pc_settings.cpp: captures the game's input and turns on pad navigation).
bool open();
// Back, from Esc (pc_settings.cpp takes the window's Escape before the settings menu would).
void back();
// UI thread, once per presented frame. The game picture is the rectangle x0,y0 to x1,y1 in the window.
void draw(float x0, float y0, float x1, float y1);

}  // namespace signin_overlay
