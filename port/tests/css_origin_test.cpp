#include "yougame_css_origin.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main() {
  using namespace yougame::css_origin;
  std::array<uint8_t,64> heap{}, image{};
  heap.fill(1); image.fill(2);
  const auto spans = native_savestate::subtract({{heap.data(),64},{image.data(),64}},{{heap.data()+8,8}});
  native_savestate::Snapshot boot;
  boot.capture(spans);
  CHECK(boot.bytes() == 120);
  const auto hash = native_savestate::hash_spans(spans);
  std::string order;
  bool disc_pending = true, callback_pending = false;
  Services s;
  s.disc_quiet = [&] { order += 'D'; return !disc_pending && !callback_pending; };
  s.restore_excluded = [&] { order += 'E'; if (heap[0] != 1 || image[0] != 2) std::abort(); };
  s.flush_gpu = [&] { order += 'G'; };
  s.reset_audio = [&] { order += 'A'; };
  s.mount_card = [&] { order += 'C'; };
  s.reset_host = [&] { order += 'H'; };
  double ms = -1;
  heap.fill(3); image.fill(4);
  CHECK(reset_to_boot(boot,s,ms) == Reset::retry);
  CHECK(order == "D" && heap[0] == 3 && image[0] == 4 && ms == -1);
  CHECK(boot.differences(1).size() == 1);
  CHECK(boot.differences(0).empty());
  disc_pending = false; callback_pending = true; order.clear();
  CHECK(reset_to_boot(boot,s,ms) == Reset::retry);
  CHECK(order == "D" && heap[0] == 3 && ms == -1);
  callback_pending = false; order.clear();
  CHECK(reset_to_boot(boot,s,ms) == Reset::restored);
  CHECK(order == "DEGACH" && ms >= 0);
  CHECK(heap[8] == 3); // Excluded bytes are the service's responsibility.
  CHECK(native_savestate::hash_spans(spans) == hash);
  CHECK(boot.differences().empty());
  heap[17] = 9; heap[18] = 9; image[60] = 8;
  auto diff = boot.differences();
  CHECK(diff.size() == 2 && diff[0].size + diff[1].size == 3);
  CHECK((diff[0].expected == 1 && diff[0].actual == 9) ||
        (diff[1].expected == 1 && diff[1].actual == 9));
  CHECK(reset_to_boot(boot,s,ms) == Reset::restored);
  CHECK(native_savestate::hash_spans(spans) == hash); // Reusable; not a consumed rollback slot.
  native_savestate::Snapshot empty;
  order.clear();
  CHECK(reset_to_boot(empty,s,ms) == Reset::unavailable && order.empty());
  std::puts("css origin: snapshot, repeated restore, bounded diff and reset ordering passed");
}
