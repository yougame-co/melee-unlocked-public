// The live game copies at S0; the existing preparation worker finishes the immutable copy.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "friends_handoff_transfer.h"
#include "handoff_state.h"
namespace yougame::console::handoff_join {
inline bool capture_base(const handoff_state::Identity& identity,
                         const std::vector<native_savestate::Span>& spans,
                         const handoff_state::State& state, Result& result, std::string& why,
                         const std::shared_ptr<Buffer>& storage = {}) {
  const auto began = transport::now_ms();
  auto copy = std::make_shared<handoff_state::Capture>();
  if (storage) copy->bytes.swap(*storage); // allocation/zeroing was done before requesting S0
  if (!handoff_state::capture(identity,spans,state,*copy,why)) return false;
  result.raw = copy->bytes.size()-state.host.size();
  std::shared_ptr<const handoff_state::Capture> immutable = std::move(copy);
  result.finish_base = [immutable](Result& out) {
    const auto began = transport::now_ms();
    auto bytes = std::make_shared<Buffer>(); handoff_state::Sizes sizes; std::string why;
    try {
      if (!handoff_state::encode_capture(*immutable,*bytes,sizes,why) || bytes->size() > stored_limit)
        out.reason = "base-capture-failed";
      else out.bytes = bytes;
    } catch (...) { out.reason = "base-capture-failed"; }
    out.worker_ms = transport::now_ms()-began;
  };
  result.ms = transport::now_ms()-began;
  return true;
}
} // namespace yougame::console::handoff_join
