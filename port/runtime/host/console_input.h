// Local hardware only. The console engine distributes these samples to every peer.
#pragma once

namespace host::console_input {
class Capture {
  bool captured_ = false;
  bool held_[4]{};
public:
  template<class Pad> void apply(Pad pads[4], bool captured) {
    for (int i = 0; i < 4; ++i) {
      // Include the closing sample: A can close the menu between hardware reads.
      if (captured || captured_) held_[i] = pads[i].button != 0;
      if (!pads[i].button || pads[i].err != 0) held_[i] = false;
      if (captured || held_[i]) {
        const auto err = pads[i].err;
        pads[i] = {};
        pads[i].err = err;
      }
    }
    captured_ = captured;
  }
};
} // namespace host::console_input
