// SPDX-License-Identifier: GPL-2.0-or-later
#include "replay_bar.h"
#include "yougame_console_menu.h"
#include "yougame_friends_lobby.h"
#include "yougame_parity.h"
#include "replay_control.h"
#include "render_options.h"
#include "host.h"
#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>

namespace replay_bar {
namespace {

constexpr double kShowSeconds = 3.0;   // how long the bar stays after the last mouse move or control
std::atomic<bool> g_cursor{false};

// Frames as minutes and seconds of play.
void clock_text(int frames, char* out, size_t size) {
  const int seconds = std::max(frames, 0) / 60;
  std::snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// H (here) or B on the controller (gx/replay_hud.cpp) flips it; "Clean replays" flips the start.
std::atomic<bool> g_hidden{false};
std::atomic<bool> g_shown{false};   // drawn this frame: the controller layer keeps the bottom clear for it
std::atomic<double> g_hidden_note{0.0};

}  // namespace

bool shown() { return g_shown.load(std::memory_order_relaxed); }
bool all_hidden() {
  return g_hidden.load(std::memory_order_relaxed) != gx::RenderOptions::live_clean_replays().load(std::memory_order_relaxed);
}
void toggle_hidden() {
  g_hidden.store(!g_hidden.load(std::memory_order_relaxed), std::memory_order_relaxed);
  g_hidden_note.store(all_hidden() ? host::now_seconds() : 0.0, std::memory_order_relaxed);
}

bool wants_cursor() { return g_cursor.load(std::memory_order_relaxed); }
bool active() { return replay_control::active(); }

void draw(float width, float height, bool menu_open) {
  const replay_control::Status st = replay_control::status();
  if (!st.active) {
    g_cursor.store(false, std::memory_order_relaxed);
    return;
  }
  replay_control::set_ui_busy(menu_open);
  const ImGuiIO& io = ImGui::GetIO();
  const double now = host::now_seconds();
  static ImVec2 last_mouse(-1.0f, -1.0f);   // UI thread only
  static double mouse_time = 0.0;
  if (ImGui::IsMousePosValid(&io.MousePos) &&
      (std::fabs(io.MousePos.x - last_mouse.x) > 1.0f || std::fabs(io.MousePos.y - last_mouse.y) > 1.0f)) {
    if (last_mouse.x >= 0.0f) mouse_time = now;   // the first position seen is not a move
    last_mouse = io.MousePos;
  }
  const bool mouse_recent = mouse_time > 0.0 && now - mouse_time < kShowSeconds;

  // H hides the bar for good (for recording): it stays away while paused, seeking or when the
  // mouse moves, and every control keeps working. H again brings it back. Read here, on the thread
  // that draws it, only while the game window has the focus and no menu has the keys.
  static bool h_was_down = true;   // UI thread only; "true" so a held H at start is no press
  bool h_down = false;
  if (!menu_open) {
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId())
      h_down = (GetAsyncKeyState('H') & 0x8000) != 0;
  }
  // "Clean replays" (Overlays) starts it hidden: H then shows it, and H again hides it.
  const bool was_hidden = all_hidden();
  if (h_down && !h_was_down) toggle_hidden();
  if (was_hidden && !all_hidden()) mouse_time = now;   // show it at once
  h_was_down = h_down || menu_open;
  const double hidden_note = g_hidden_note.load(std::memory_order_relaxed);
  if (all_hidden()) {
    g_cursor.store(false, std::memory_order_relaxed);
    g_shown.store(false, std::memory_order_relaxed);
    if (hidden_note > 0.0 && now - hidden_note < 2.0) {
      // Said once, briefly, so the key that brings it back is not a secret.
      const char* note = "Replay controls hidden. H or B: show them again";
      const ImVec2 size = ImGui::CalcTextSize(note);
      const ImVec2 at(16.0f, height - size.y - 20.0f);
      ImDrawList* nl = ImGui::GetForegroundDrawList();
      nl->AddRectFilled(ImVec2(at.x - 8.0f, at.y - 5.0f), ImVec2(at.x + size.x + 8.0f, at.y + size.y + 5.0f),
                        IM_COL32(10, 12, 18, 190), 5.0f);
      nl->AddText(at, IM_COL32(236, 240, 246, 255), note);
    }
    return;
  }
  // With the controller layer up (gx/replay_hud.h), which has its own timeline and buttons, the bar
  // comes up only for the mouse.
  const bool visible = !menu_open && (mouse_recent || (!replay_control::pad_layer() &&
                                      (st.paused || st.seeking || (st.last_input > 0.0 && now - st.last_input < kShowSeconds))));
  g_cursor.store(visible && mouse_recent, std::memory_order_relaxed);
  g_shown.store(visible, std::memory_order_relaxed);
  if (!visible) return;

  const float text_h = ImGui::GetTextLineHeight();
  const float bar_w = width - 32.0f, bar_h = 2.0f * text_h + 34.0f;
  if (bar_w < 320.0f || height < bar_h + 40.0f) return;
  const ImVec2 o(16.0f, height - bar_h - 12.0f);
  const float line_x0 = o.x + 12.0f, line_x1 = o.x + bar_w - 12.0f;
  const float line_y = o.y + 8.0f + text_h + 9.0f;

  const ImU32 back = IM_COL32(10, 12, 18, 190), track = IM_COL32(70, 76, 92, 255);
  const ImU32 fill = IM_COL32(155, 107, 255, 255), text = IM_COL32(236, 240, 246, 255);
  const ImU32 dim = IM_COL32(160, 168, 184, 255), mark = IM_COL32(250, 214, 60, 255);

  const int total = std::max(1, st.last_frame - st.first_frame);
  const int at = std::clamp(st.frame - st.first_frame, 0, total);
  const float done = (float)at / (float)total;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(o, ImVec2(o.x + bar_w, o.y + bar_h), back, 6.0f);
  dl->PushClipRect(o, ImVec2(o.x + bar_w, o.y + bar_h), true);

  // Time, then what playback is doing.
  char now_text[16], total_text[16], line[160];
  clock_text(at, now_text, sizeof now_text);
  clock_text(total, total_text, sizeof total_text);
  char state[64];
  if (st.seeking) {
    char target[16];
    clock_text(st.seek_target - st.first_frame, target, sizeof target);
    std::snprintf(state, sizeof state, "Seeking to %s", target);
  } else if (st.paused) std::snprintf(state, sizeof state, "Paused");
  else if (st.fast) std::snprintf(state, sizeof state, "Fast forward 4x");
  else if (st.speed < 0.375) std::snprintf(state, sizeof state, "Slow motion 1/4");
  else if (st.speed < 0.75) std::snprintf(state, sizeof state, "Slow motion 1/2");
  else std::snprintf(state, sizeof state, "Playing");
  std::snprintf(line, sizeof line, "%s / %s     %s%s", now_text, total_text, state,
                st.as_played ? "     shown as it was played" : "");
  dl->AddText(ImVec2(line_x0, o.y + 8.0f), text, line);

  // The progress line. A click on it jumps there.
  const bool over = ImGui::IsMousePosValid(&io.MousePos) && io.MousePos.x >= line_x0 - 4.0f && io.MousePos.x <= line_x1 + 4.0f &&
                    io.MousePos.y >= line_y - 10.0f && io.MousePos.y <= line_y + 10.0f;
  const float half = over ? 3.5f : 2.0f;
  dl->AddRectFilled(ImVec2(line_x0, line_y - half), ImVec2(line_x1, line_y + half), track, half);
  const float done_x = line_x0 + (line_x1 - line_x0) * done;
  dl->AddRectFilled(ImVec2(line_x0, line_y - half), ImVec2(done_x, line_y + half), fill, half);
  dl->AddCircleFilled(ImVec2(done_x, line_y), over ? 7.0f : 5.0f, fill);
  if (st.seeking) {
    const float target_x = line_x0 + (line_x1 - line_x0) *
                                         std::clamp((float)(st.seek_target - st.first_frame) / (float)total, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(target_x - 1.0f, line_y - 7.0f), ImVec2(target_x + 1.0f, line_y + 7.0f), mark);
  }
  dl->AddText(ImVec2(line_x0, line_y + 10.0f), dim,
              "Space: pause     Left, Right: 5 seconds (Shift: 30)     Right or . while paused: one frame     "
              "[ ] or Down, Up: speed     Tab or R: fast forward     Home: restart     H: hide everything");
  dl->PopClipRect();

  if (over) {
    // The time under the pointer, just above the bar.
    const float fraction = std::clamp((io.MousePos.x - line_x0) / (line_x1 - line_x0), 0.0f, 1.0f);
    const int frame = (int)(fraction * (float)total);
    char there[16];
    clock_text(frame, there, sizeof there);
    const float tw = ImGui::CalcTextSize(there).x;
    const ImVec2 at_text(std::clamp(io.MousePos.x - tw * 0.5f, line_x0, line_x1 - tw), o.y - text_h - 8.0f);
    dl->AddRectFilled(ImVec2(at_text.x - 6.0f, at_text.y - 3.0f), ImVec2(at_text.x + tw + 6.0f, at_text.y + text_h + 3.0f), back, 4.0f);
    dl->AddText(at_text, mark, there);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) replay_control::seek_to(st.first_frame + frame);
  }
}

}  // namespace replay_bar

namespace screen_label {
namespace {
std::mutex g_lock;
std::string g_text;
double g_until = 0.0;
// UI thread: where the corner hint box ended, and the ImGui frame that drew it (it is not drawn every frame).
float g_hint_bottom = 0.0f;
int g_hint_frame = -2;
// UI thread: top of the row that sits on the picture's bottom edge (the Friends waiting row), and its frame.
float g_row_top = 0.0f;
int g_row_frame = -2;
}  // namespace

void show(const char* text, double seconds) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_text = text ? text : "";
  g_until = host::now_seconds() + seconds;
}

namespace {
struct CornerHint { float font; ImVec2 at, size, min, max; };
CornerHint corner_hint(float width, float height, float aspect, bool local_friends = false) {
  const float ph = std::min(height,width/std::max(aspect,0.1f));
  const float pw = ph*std::max(aspect,0.1f), left = (width-pw)*0.5f, top = (height-ph)*0.5f;
  const float margin = std::min(7.0f,pw*0.01f);
  // CSS already has a compact Friends header in the top margin. Match that scale so
  // the two hint rows fit above the retail header rather than covering its title.
  const bool compact = local_friends || yougame::console::console_menu_visible();
  float fs = compact ? ph*9.0f/480.0f : std::clamp(ph/38.0f,12.0f,18.0f);
  auto* font = ImGui::GetFont();
  auto size = font->CalcTextSizeA(fs,FLT_MAX,0,yougame::parity::kMenuHint);
  const float available = std::max(0.1f,pw-4*margin);
  if (size.x > available) { fs *= available/size.x; size = font->CalcTextSizeA(fs,FLT_MAX,0,yougame::parity::kMenuHint); }
  const float pad = compact ? ph/480.0f : 1.0f;
  const ImVec2 at(left+pw-size.x-2*margin,top+(compact ? ph*0.003f : std::min(10.0f,ph*0.02f)));
  return {fs,at,size,ImVec2(at.x-margin,at.y-3*pad),ImVec2(at.x+size.x+margin,at.y+fs+4*pad)};
}
// The console menu notice, under the compact "Friends" heading. Sized in picture lines (u = 1/480 of the
// picture) so its gap to the retail "UCF" label (from line ~26) holds at page size as well as full screen:
// pixel padding put it over the label in a small picture. It keeps its row and makes room for the corner hint
// sideways: it slides left, then narrows (scale). Only a picture too narrow for both drops it under the hint.
struct MenuNotice { ImVec2 at, min, max; float scale; };
MenuNotice menu_notice(float width, float height, float aspect, ImVec2 size) {
  const float ph = std::min(height,width/std::max(aspect,0.1f)), pw = ph*std::max(aspect,0.1f);
  const float left = (width-pw)*0.5f, top = (height-ph)*0.5f, u = ph/480.0f;
  ImVec2 at((width-size.x)*0.5f,top+12.4f*u);
  float scale = 1.0f;
  const auto corner = corner_hint(width,height,aspect);
  if (at.y-u < corner.max.y && at.x+size.x+2*u > corner.min.x-u) {
    const float room = corner.min.x-u-2*u-(left+4*u);
    if (room >= size.x*0.6f) {
      scale = std::min(1.0f,room/std::max(size.x,0.1f));
      at.x = std::max(left+4*u,std::min(at.x,corner.min.x-3*u-size.x*scale));
    } else {
      at.y = corner.max.y+2*u;
    }
  }
  const ImVec2 drawn(size.x*scale,size.y*scale);
  return {at,ImVec2(at.x-2*u,at.y-u),ImVec2(at.x+drawn.x+2*u,at.y+drawn.y+u),scale};
}
}
void draw_corner_hint(float width, float height, float aspect) {
  static yougame::parity::CornerHintFade fade;
  const auto ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto toast = yougame::friends::events.toast(ms), message = yougame::friends::events.message(ms);
  const float alpha = fade.alpha(toast.empty() && message.empty() ? std::string() : toast + "\n" + message,
                                 host::now_seconds());
  g_hint_bottom = 0.0f;
  if (alpha <= 0) return;
  const auto h = corner_hint(width,height,aspect,yougame::friends::lobby.host());
  g_hint_bottom = h.max.y; g_hint_frame = ImGui::GetFrameCount();
  auto* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(h.min,h.max,IM_COL32(9,11,22,int(190*alpha)),5);
  dl->AddText(ImGui::GetFont(),h.font,h.at,IM_COL32(236,238,246,int(235*alpha)),yougame::parity::kMenuHint);
}
void reserve_bottom(float top) { g_row_top = top; g_row_frame = ImGui::GetFrameCount(); }
float corner_hint_bottom() { return ImGui::GetFrameCount() - g_hint_frame <= 1 ? g_hint_bottom : 0.0f; }

void draw(float width, float height, float game_aspect) {
  static yougame::console::MenuHintAnimation animation;
  const float alpha = animation.alpha(yougame::console::console_menu_visible(),host::now_seconds());
  if (alpha > 0) {
    // The retail CSS header meets the grid with no spare text row. A compact host "Friends"
    // header and its hint use the picture's empty top margin, above all retail labels/cards.
    // Use the actual presentation aspect (73:60, 16:9, etc.), including letterboxing.
    const float picture_height = std::min(height,width/std::max(game_aspect,0.1f));
    const float top = (height-picture_height)*0.5f;
    float font_size = picture_height*9.0f/480.0f;
    const auto* hint = yougame::console::kMenuHint;
    ImFont* font = ImGui::GetFont();
    ImVec2 size = font->CalcTextSizeA(font_size,FLT_MAX,0,hint);
    if (size.x > width*0.96f) { font_size *= width*0.96f/size.x; size = font->CalcTextSizeA(font_size,FLT_MAX,0,hint); }
    const auto notice = menu_notice(width,height,game_aspect,size);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 heading = font->CalcTextSizeA(font_size,FLT_MAX,0,"Friends");
    dl->AddText(font,font_size,ImVec2((width-heading.x)*0.5f,top+picture_height*0.003f),
                IM_COL32(236,240,246,int(255*alpha)),"Friends");
    dl->AddRectFilled(notice.min,notice.max,IM_COL32(10,12,18,int(170*alpha)),picture_height/400.0f);
    dl->AddText(font,font_size*notice.scale,notice.at,IM_COL32(255,239,170,int(255*alpha)),hint);
  }
  // With the app's lobby-lines cap the page draws this line outside the picture.
  const auto friends_line = yougame::friends::page_lobby_lines.load(std::memory_order_acquire) ? std::string() :
                            yougame::friends::lobby.line();
  if (!friends_line.empty()) {
    const float ph = std::min(height,width/std::max(game_aspect,0.1f));
    const auto corner = corner_hint(width,height,game_aspect,true);
    auto* font = ImGui::GetFont();
    float fs = ph*9.0f/480.0f;
    auto size = font->CalcTextSizeA(fs,FLT_MAX,0,friends_line.c_str());
    if (size.x > width*.96f) { fs *= width*.96f/size.x; size = font->CalcTextSizeA(fs,FLT_MAX,0,friends_line.c_str()); }
    const ImVec2 at((width-size.x)*.5f,corner.max.y+2+ph/480.0f);
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(at.x-3,at.y-1),ImVec2(at.x+size.x+3,at.y+size.y+1),IM_COL32(10,12,18,180),2);
    dl->AddText(font,fs,at,IM_COL32(255,239,170,255),friends_line.c_str());
  }
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_text.empty() || host::now_seconds() >= g_until) return;
    text = g_text;
  }
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  // Above the bottom-edge row while it shows, so a toast never covers "Waiting for your friend...".
  float bottom = height - 28.0f;
  if (ImGui::GetFrameCount() - g_row_frame <= 1) bottom = std::min(bottom, g_row_top - 8.0f);
  const ImVec2 at((width - size.x) * 0.5f, bottom - size.y);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(at.x - 10.0f, at.y - 6.0f), ImVec2(at.x + size.x + 10.0f, at.y + size.y + 6.0f),
                    IM_COL32(10, 12, 18, 200), 6.0f);
  dl->AddText(at, IM_COL32(236, 240, 246, 255), text.c_str());
}
}  // namespace screen_label
