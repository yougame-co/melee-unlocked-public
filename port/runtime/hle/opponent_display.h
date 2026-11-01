// Presentation only. Never store these labels in player identities or replay metadata.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace slippi::display {

enum class Provider { Unknown, Slippi, YouGame };

// Width is in UTF-8 code points, matching convert_string_for_game's character limit.
// U+00B7 is not representable by the Mac/Linux CP932 conversion. ASCII '-' is
// explicitly converted to the game's Shift-JIS hyphen glyph (0x817c).
inline std::string opponent_label(Provider provider, std::string_view name,
                                  size_t width = std::string::npos) {
  const std::string_view tag = provider == Provider::Slippi ? " - Slippi" : "";
  // An empty identity has no label; a width smaller than the tag cannot show it whole.
  if (name.empty() || width < tag.size()) return {};
  const size_t name_width = width - tag.size();
  size_t end = 0, count = 0;
  while (end < name.size() && count < name_width) {
    ++end;
    while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) ++end;
    ++count;
  }
  std::string text(name.substr(0, end));
  text.append(tag.data(), tag.size());
  return text;
}

}  // namespace slippi::display
