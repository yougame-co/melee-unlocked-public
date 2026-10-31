// Presentation only. Never store these labels in player identities or replay metadata.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace slippi::display {

enum class Provider { Unknown, Slippi, YouGame };

// Width is in UTF-8 code points, matching convert_string_for_game's character limit.
// ASCII separators also work with the game's Shift-JIS font.
inline std::string opponent_label(Provider provider, std::string_view name,
                                  size_t width = std::string::npos) {
  std::string text;
  switch (provider) {
    case Provider::Slippi: text = "Slippi"; break;
    case Provider::YouGame: text = "YouGame"; break;
    case Provider::Unknown: break;
  }
  if (!text.empty() && !name.empty()) text += ' ';
  text.append(name.data(), name.size());
  size_t end = 0, count = 0;
  while (end < text.size() && count < width) {
    ++end;
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
    ++count;
  }
  text.resize(end);
  return text;
}

}  // namespace slippi::display
