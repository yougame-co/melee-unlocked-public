// Pure candidate policy test: no ENet, sockets, account, or server.
#include "slippi_address.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  int checks=0; using namespace slippi;
  auto a=remote_address("203.0.113.1","203.0.113.1:42000","192.168.1.2:41000");
  CHECK(a.first=="192.168.1.2:41000");CHECK(a.fallback=="203.0.113.1:42000");
  CHECK(!public_fallback_due(0,false,false,true));
  CHECK(!public_fallback_due(1999,false,false,true));
  CHECK(public_fallback_due(2000,false,false,true));
  CHECK(public_fallback_due(2500,false,false,true));
  CHECK(!public_fallback_due(2000,true,false,true));
  CHECK(!public_fallback_due(2000,false,true,true));
  CHECK(!public_fallback_due(2000,false,false,false));
  auto b=remote_address("203.0.113.1","203.0.113.2:43000","192.168.1.2:41000");
  CHECK(b.first=="203.0.113.2:43000" && b.fallback.empty());
  auto c=remote_address("203.0.113.1","203.0.113.1:44000","");
  CHECK(c.first=="203.0.113.1:44000" && c.fallback.empty());
  auto d=remote_address("203.0.113.1","203.0.113.1:44000","203.0.113.1:44000");
  CHECK(d.first=="203.0.113.1:44000" && d.fallback.empty());
  struct Addr { uint32_t host; uint16_t port; } e{};
  CHECK(public_endpoint(std::string("203.0.113.1:42000"),e));
  { const uint8_t want[4]={203,0,113,1}; CHECK(std::memcmp(&e.host,want,4)==0); CHECK(e.port==42000); }
  CHECK(!public_endpoint(std::string("203.0.113.1"),e));
  CHECK(!public_endpoint(std::string("203.0.113:42000"),e));
  CHECK(!public_endpoint(std::string("203.0.113.256:42000"),e));
  CHECK(!public_endpoint(std::string("203.0.113.1:0"),e));
  CHECK(!public_endpoint(std::string("203.0.113.1:65536"),e));
  CHECK(!public_endpoint(std::string("mm.slippi.gg:43113"),e));
  CHECK(!public_endpoint(std::string("1.2.3.4.5:80"),e));
  CHECK(!public_endpoint(std::string("1.2.3.4:8x"),e));
  std::printf("Slippi address policy: %d checks passed\n",checks);
}
