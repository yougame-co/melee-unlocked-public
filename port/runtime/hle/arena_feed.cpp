// Arena spectator feed (see arena_feed.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_feed.h"
#include <cstring>
#include <ctime>
#include <filesystem>

namespace slippi::arena_feed {
namespace {

void put16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(v >> shift));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
int32_t frame_of(const uint8_t* p) {
  const uint32_t bits = get32(p);
  int32_t frame;
  std::memcpy(&frame, &bits, sizeof frame);
  return frame;
}
// Every message starts with its kind and the game's tag.
std::vector<uint8_t> header(Kind kind, uint32_t tag) {
  std::vector<uint8_t> m = {uint8_t(kind)};
  put32(m, tag);
  return m;
}
constexpr size_t kHeader = 5;

}  // namespace

uint32_t game_tag(const uint8_t* start, size_t size) {
  uint32_t h = 2166136261u;
  const size_t n = size < 0x1A0 ? size : 0x1A0;   // the game's settings and seed; names after differ per side
  for (size_t i = 0; i < n; ++i) { h ^= start[i]; h *= 16777619u; }
  return h ? h : 1;
}

int my_result(const uint8_t* end, size_t size, int my_port, const uint8_t port_team[4], bool doubles) {
  // Methods 1 time, 2 game and 3 (game, newer recordings) have a result; 7 is a no contest.
  if (!end || size < 6 || end[0] < 1 || end[0] > 3 || my_port < 0 || my_port > 3) return -1;
  int first = -1, count = 0;
  for (int port = 0; port < 4; ++port) if (end[2 + port] == 0) { first = port; ++count; }
  if (count != 1) return -1;
  if (doubles) return port_team[first] == port_team[my_port] ? 1 : 0;
  return first == my_port ? 1 : 0;
}

void port_teams(const uint8_t* start, size_t size, uint8_t out[4]) {
  // The game info block follows the version word; player blocks of 0x24 bytes at 0x60, the team at +9.
  for (int port = 0; port < 4; ++port) {
    const size_t at = 4 + 0x60 + 0x24 * (size_t)port + 9;
    out[port] = start && at < size ? start[at] : 0;
  }
}

// ---------------------------------------------------------------- sender
void Sender::send(const std::vector<uint8_t>& message) {
  stats_.bytes += message.size();
  ++stats_.messages;
  if (out_) out_(message.data(), uint32_t(message.size()));
}

// Sends the waiting frames up to `through`, in order.
void Sender::release(int32_t through) {
  while (!waiting_.empty() && waiting_.begin()->first <= through) {
    const int32_t frame = waiting_.begin()->first;
    if (any_sent_ && frame > next_) break;   // a frame is missing before it: nothing passes it
    std::vector<uint8_t> m = header(kFrame, tag_);
    put32(m, uint32_t(frame));
    const std::vector<uint8_t>& events = waiting_.begin()->second;
    put16(m, uint16_t(events.size()));
    m.insert(m.end(), events.begin(), events.end());
    send(m);
    ++stats_.frames;
    any_sent_ = true;
    next_ = frame + 1;
    waiting_.erase(waiting_.begin());
  }
}

void Sender::event(const uint8_t* data, uint32_t size) {
  if (!data || !size) return;
  const uint8_t command = data[0];
  const uint8_t* payload = data + 1;
  const uint32_t length = size - 1;
  switch (command) {
    case 0x35: {   // the command table: this recording's event sizes
      for (uint32_t i = 1; i + 2 < length; i += 3) {
        const uint16_t value = get16(payload + i + 1);
        if (payload[i] == 0x37) pre_size_ = value;
        if (payload[i] == 0x38) post_size_ = value;
      }
      break;
    }
    case 0x36: {   // Game Start: a new match
      tag_ = fixed_tag_ ? fixed_tag_ : game_tag(payload, length);
      started_ = true;
      open_ = false;
      waiting_.clear();
      any_sent_ = false;
      next_ = kFirstFrame;
      std::vector<uint8_t> m = header(kStart, tag_);
      m.push_back(kVersion);
      put16(m, pre_size_);
      put16(m, post_size_);
      put16(m, uint16_t(length));
      m.insert(m.end(), payload, payload + length);
      send(m);
      break;
    }
    case 0x3A: {   // frame start
      if (!started_ || length < 4) break;
      open_ = true;
      open_frame_ = frame_of(payload);
      open_events_.assign(data, data + size);
      break;
    }
    case 0x37:     // pre-frame
    case 0x38: {   // post-frame
      if (!open_ || length < 4 || frame_of(payload) != open_frame_) break;
      if (command == 0x38 && !full_post_ && open_frame_ != kFirstFrame) break;
      open_events_.insert(open_events_.end(), data, data + size);
      break;
    }
    case 0x3C: {   // frame end: this frame is complete, and frames up to `final` will not change
      if (!open_ || length < 8 || frame_of(payload) != open_frame_) break;
      open_ = false;
      const int32_t final_frame = frame_of(payload + 4);
      if (any_sent_ && open_frame_ < next_) {
        ++stats_.late;   // already sent as final and simulated again: the game broke its own word
      } else {
        if (waiting_.count(open_frame_)) ++stats_.superseded;
        waiting_[open_frame_] = std::move(open_events_);
        if (waiting_.size() > stats_.max_waiting) stats_.max_waiting = waiting_.size();
      }
      open_events_.clear();
      release(final_frame);
      break;
    }
    case 0x39: {   // Game End: what is still waiting is as final as it gets
      if (!started_) break;
      open_ = false;
      if (!waiting_.empty()) release(waiting_.rbegin()->first);
      waiting_.clear();
      std::vector<uint8_t> m = header(kEnd, tag_);
      m.push_back(uint8_t(length > 255 ? 0 : length));
      if (length <= 255) m.insert(m.end(), payload, payload + length);
      send(m);
      started_ = false;
      break;
    }
    default:
      break;   // items, stage events, the code list: not needed to play the match
  }
}

void Sender::stop() {
  if (!started_) return;
  started_ = false;
  open_ = false;
  waiting_.clear();
  std::vector<uint8_t> m = header(kEnd, tag_);
  m.push_back(0);
  send(m);
}

// ---------------------------------------------------------------- receiver
bool Receiver::fail(const char* reason) {
  error_ = reason;
  return false;
}

bool Receiver::open(const std::string& path, std::vector<uint8_t> codes, uint32_t game_tag) {
  close();
  *this = Receiver{};
  codes_ = std::move(codes);
  tag_ = game_tag;
  file_.open(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
  if (!file_.is_open()) return fail("cannot create the live recording");
  return true;
}

void Receiver::close() {
  if (file_.is_open()) file_.close();
}

bool Receiver::message(int source, const uint8_t* data, uint32_t size) {
  if (!data || size < kHeader) return fail("message too short");
  if (!file_.is_open() && !ended_) return fail("no live recording is open");
  if (tag_ == 0 && data[0] == kStart) tag_ = get32(data + 1);
  if (get32(data + 1) != tag_) return true;   // another game's
  const uint8_t* p = data + kHeader;
  const uint32_t n = size - uint32_t(kHeader);
  switch (data[0]) {
    case kStart: {
      if (started_ || ended_) return true;   // the other fighter's copy
      if (n < 7) return fail("start message too short");
      if (p[0] != kVersion) return fail("feed version differs");
      if (get16(p + 1) != kPreFrameSize || get16(p + 3) != kPostFrameSize) return fail("recording layout differs");
      const uint16_t length = get16(p + 5);
      if (n != 7u + length) return fail("start message length");
      // A recording still being written: the file's own header with no length yet, then the events.
      static const uint8_t head[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0};
      file_.write(reinterpret_cast<const char*>(head), sizeof head);
      stream_.set_event_sink([this](const uint8_t* bytes, uint32_t count) {
        file_.write(reinterpret_cast<const char*>(bytes), count);
      });
      const std::vector<uint8_t> start(p + 7, p + 7 + length);
      if (!stream_.begin_slippi(start, codes_, int64_t(std::time(nullptr)))) return fail("cannot start the live recording");
      file_.flush();
      started_ = true;
      return true;
    }
    case kFrame: {
      if (!started_ || ended_) return true;
      if (n < 6) return fail("frame message too short");
      const int32_t frame = frame_of(p);
      const uint16_t length = get16(p + 4);
      if (n != 6u + length) return fail("frame message length");
      if (frame < next_) { ++stats_.duplicates; return true; }
      if (frame > next_) { ++stats_.gaps; return true; }
      // Check the whole frame before any of it is written.
      const uint8_t* events = p + 6;
      struct Fighter { uint8_t slot, follower; };
      std::vector<Fighter> fighters;
      bool has_post = false;
      for (uint32_t at = 0; at < length;) {
        const uint8_t command = events[at];
        const uint16_t need = command == 0x3A ? kFrameStartSize : command == 0x37 ? kPreFrameSize
                            : command == 0x38 ? kPostFrameSize : 0;
        if (!need || at + 1u + need > length) return fail("frame message holds an event it should not");
        if ((at == 0) != (command == 0x3A)) return fail("frame message does not open with a frame start");
        if (frame_of(events + at + 1) != frame) return fail("frame message mixes frames");
        if (command == 0x37) fighters.push_back({events[at + 5], events[at + 6]});
        if (command == 0x38) has_post = true;
        at += 1u + need;
      }
      if (!length) return fail("empty frame message");
      for (uint32_t at = 0; at < length;) {
        const uint8_t command = events[at];
        const uint16_t need = command == 0x3A ? kFrameStartSize : command == 0x37 ? kPreFrameSize : kPostFrameSize;
        if (command == 0x38) character_[uint16_t(events[at + 5] << 8 | events[at + 6])] = events[at + 7];
        if (!stream_.append(command, events + at + 1, need)) return fail("cannot write the frame");
        at += 1u + need;
      }
      if (!has_post) {
        // The viewer takes a frame once a post-frame event says its inputs are all there. The feed
        // leaves those out, so each fighter gets a blank one here: its frame, slot and character.
        for (const Fighter& f : fighters) {
          uint8_t post[kPostFrameSize] = {};
          std::memcpy(post, events + 1, 4);
          post[4] = f.slot;
          post[5] = f.follower;
          const auto known = character_.find(uint16_t(f.slot << 8 | f.follower));
          post[6] = known == character_.end() ? 0 : known->second;
          if (!stream_.append(0x38, post, sizeof post)) return fail("cannot write the frame");
        }
      }
      uint8_t end[8];
      std::memcpy(end, events + 1, 4);
      std::memcpy(end + 4, events + 1, 4);   // everything written here is final
      if (!stream_.append(0x3C, end, sizeof end)) return fail("cannot write the frame");
      file_.flush();
      next_ = frame + 1;
      ++stats_.frames;
      if (source >= 0 && source < 4) ++stats_.from[source];
      return true;
    }
    case kEnd: {
      if (!started_ || ended_) return true;
      if (n < 1 || n != 1u + p[0]) return fail("end message length");
      if (p[0] == kGameEndSize) {
        if (!stream_.append(0x39, p + 1, kGameEndSize)) return fail("cannot write Game End");
        ended_ = true;
        file_.flush();
        close();
      }
      // An end with no Game End is one fighter leaving; the other may still be sending.
      return true;
    }
    default:
      return fail("unknown message");
  }
}

}  // namespace slippi::arena_feed
