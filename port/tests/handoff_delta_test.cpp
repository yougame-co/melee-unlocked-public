// SPDX-License-Identifier: GPL-2.0-or-later
#include "handoff_state.h"
#include <chrono>
#include <cstring>
#include <random>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#x,why.c_str()); return 1; } } while (0)
using namespace handoff_state;
int main(int argc,char**) {
  unsigned checks = 0; std::string why;
  const bool budget = argc > 1;
  std::vector<uint8_t> memory(budget ? 40*1024*1024 : 65537,0x31);
  std::mt19937 rng(42);
  for (size_t i = 0; i < (budget ? 256*1024 : memory.size()); ++i) memory[i] = uint8_t(rng());
  std::vector<native_savestate::Span> spans{{memory.data(),12345},{memory.data()+12345,uint32_t(memory.size()-12345)}};
  Identity id{123,{}}; State state{907,2,0,{1,2,3}}; Sizes sizes;
  auto clock = [] { return std::chrono::steady_clock::now(); };
  auto ms = [&](auto a) { return std::chrono::duration<double,std::milli>(clock()-a).count(); };
  std::vector<uint8_t> base,delta,target;
  auto began = clock(); CHECK(encode(id,spans,state,base,sizes,why)); const auto base_ms = ms(began);
  const auto original = memory;
  for (size_t i : {size_t(0),size_t(4095),size_t(4096),memory.size()-1}) memory[i] ^= 0x7f;
  if (budget) for (size_t i = 0; i < 32; ++i) memory[1024*1024+i*4096] ^= uint8_t(i+1);
  ++state.frame; state.host = {4,5,6,7};
  size_t pages = 0;
  began = clock(); CHECK(encode_delta(base,id,spans,state,delta,pages,why)); const auto delta_ms = ms(began);
  CHECK(pages == (budget ? 35 : 3));
  const auto wanted = memory;
  began = clock(); CHECK(apply_delta(base,delta,id,spans,target,why)); const auto apply_ms = ms(began);
  CHECK(memory == wanted); // reconstruction must NEVER write the live spans
  memory = original;
  State loaded;
  began = clock(); CHECK(load(target,id,spans,loaded,sizes,why)); const auto load_ms = ms(began);
  CHECK(memory == wanted && loaded.frame == state.frame && loaded.host == state.host);
  began = clock(); const auto hash = native_savestate::hash_spans(spans); const auto hash_ms = ms(began);
  if (budget) {
    std::printf("synthetic 40 MiB: base %zu bytes %.3f ms; delta %zu pages %zu bytes %.3f ms; reconstruct %.3f ms; load %.3f ms; hash %.3f ms (%016llx)\n",
        base.size(),base_ms,pages,delta.size(),delta_ms,apply_ms,load_ms,hash_ms,(unsigned long long)hash);
    return 0;
  }
  auto reject = [&](const std::vector<uint8_t>& b,const std::vector<uint8_t>& d,Identity identity) {
    const auto before = memory; std::vector<uint8_t> out{9};
    return !apply_delta(b,d,identity,spans,out,why) && memory == before && out.empty();
  };
  auto bad = base; bad[52] ^= 1;
  CHECK(reject(bad,delta,id) && why == "wrong delta base");
  auto wrong_id = id; ++wrong_id.size;
  CHECK(reject(base,delta,wrong_id) && why == "different game library identity");
  for (size_t n = 0; n < 88; ++n) {
    bad.assign(delta.begin(),delta.begin()+n); CHECK(reject(base,bad,id));
  }
  for (size_t n : {delta.size()/2,delta.size()-8,delta.size()-1}) {
    bad.assign(delta.begin(),delta.begin()+n); CHECK(reject(base,bad,id));
  }
  for (size_t at : {size_t(0),size_t(4),size_t(8),size_t(40),size_t(48),size_t(56),size_t(88)}) {
    bad = delta; bad[at] ^= 1; CHECK(reject(base,bad,id));
  }
  bad = delta; std::fill(bad.begin()+40,bad.begin()+48,255); CHECK(reject(base,bad,id));
  bad = delta; bad.push_back(0); CHECK(reject(base,bad,id));
  bad.assign(join_stored_limit+1,0); CHECK(reject(base,bad,id));
  CHECK(reject(bad,delta,id));
  CHECK(encode_delta(base,id,spans,state,delta,pages,why));
  CHECK(pages == 3); // still differs from original base
  memory = original;
  CHECK(encode_delta(base,id,spans,state,delta,pages,why) && pages == 0);
  CHECK(apply_delta(base,delta,id,spans,target,why));
  CHECK(load(target,id,spans,loaded,sizes,why) && memory == original);
  // Dense/incompressible and final short page, with distinct span boundaries.
  for (auto& b : memory) b = uint8_t(rng());
  CHECK(encode_delta(base,id,spans,state,delta,pages,why) && pages == 17);
  CHECK(apply_delta(base,delta,id,spans,target,why));
  std::printf("handoff delta: %u checks passed\n",checks); return 0;
}
