// Offline three-machine, four-port regression using the production packet and queue paths.
#include "slippi_port_inputs.h"
#include "native_slippi_bridge.h"
#include "yougame_lobby_setup.h"
#include "yougame_friends.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using namespace slippi;

static Pad sample(int frame, int port) {
  Pad p(frame); p.checksum_frame = frame - 2; p.checksum = uint32_t(frame * 771);
  // Slippi 3.7.0's wire pad: the inputs and the RNG bits byte, both carried for every port.
  for (int i = 0; i < PAD_WIRE_SIZE; ++i) p.buf[i] = frame <= 2 ? 0 : uint8_t(frame * 17 + port * 31 + i);
  return p;
}
static void simulation(std::array<int,4> owners, std::vector<int> machines, int humans) {
  PortInputs inputs[3];
  std::deque<std::unique_ptr<Pad>> sent[3];
  std::vector<uint8_t> previous[3];
  for (size_t m = 0; m < machines.size(); ++m) {
    inputs[m].main = machines[m]; inputs[m].humans = humans; inputs[m].owner = owners;
  }
  for (int frame = 1; frame <= 180; ++frame) {
    for (size_t m = 0; m < machines.size(); ++m) {
      auto& bank = inputs[m]; const int main = bank.main, partner = bank.partner(main);
      const auto pad = sample(frame,main);
      sent[m].push_front(std::make_unique<Pad>(pad));
      if (partner >= 0) {
        bank.local_partner(sample(frame,partner));
        // The delay is identical to the main's: frames 1-2 neutral, then target = sample + 2.
        CHECK(bank.pads[partner].front()->frame == frame);
        CHECK(!std::memcmp(bank.pads[partner].front()->buf,sample(frame,partner).buf,PAD_DATA_SIZE));
        CHECK(bank.virtual_local(bank.index(partner)));
      }
      while (sent[m].size() > 128) sent[m].pop_back();
      auto packet = bank.packet(sent[m]); CHECK(!packet.empty());
      for (size_t other = 0; other < machines.size(); ++other) {
        if (other == m) continue;
        if (frame % 7 == 0 && frame != 180) continue; // dropped packet: next one carries both histories
        const auto r = inputs[other].receive(packet.data(),packet.size(),main);
        CHECK(r.ok && r.frame == frame && r.checksum == pad.checksum);
        if (!previous[m].empty()) CHECK(inputs[other].receive(previous[m].data(),previous[m].size(),main).ok); // reordered duplicate
      }
      previous[m] = packet;
    }
  }
  // Every physical machine has identical confirmed inputs for every port. Local main uses the
  // game's delay history; local partner uses the very same queue read by rollback on other machines.
  for (size_t m = 0; m < machines.size(); ++m) {
    for (int port = 0; port < humans; ++port) {
      if (port == inputs[m].main) continue;
      const auto& q = inputs[m].pads[port]; CHECK(q.size() == 180);
      for (int frame = 1; frame <= 180; ++frame) {
        const auto expected = sample(frame,port);
        CHECK(q[180-frame]->frame == frame);
        CHECK(!std::memcmp(q[180-frame]->buf,expected.buf,PAD_WIRE_SIZE));
      }
    }
  }
  // A real connection's activity controls all its ports, even before the first input arrives.
  for (size_t m = 1; m < machines.size(); ++m) {
    bool live[4] = {true,true,true,true};
    const auto active = [&](int p) { return live[p]; };
    CHECK(inputs[m].active(0,active) && inputs[m].active(1,active));
    live[0] = false;
    CHECK(!inputs[m].active(0,active) && !inputs[m].active(1,active));
  }
  const auto local = inputs[0].local_output(inputs[0].index(1),178,7);
  CHECK(local.latest_frame == 178 && !local.is_disconnected && !local.checksum_primary);
  CHECK(local.data.size() == 7 * PAD_FULL_SIZE);
  for (int i = 0; i < 7; ++i) CHECK(!std::memcmp(local.data.data()+i*PAD_FULL_SIZE,sample(178-i,1).buf,PAD_DATA_SIZE));
  // A slow ACK may retain 129 send frames while rollback has finalized all but seven. Sending
  // must still carry both ports, and reading the owner's partner must start at the rollback frontier.
  auto& owner = inputs[0];
  while (owner.pads[1].size() > 7) owner.pads[1].pop_back();
  owner.trim_send(sent[0].back()->frame);
  auto retained = owner.packet(sent[0]);
  CHECK(!retained.empty() && owner.pads[1].back()->frame == 174 && owner.partner_send.back()->frame < 174);
  CHECK(inputs[1].receive(retained.data(),retained.size(),0).ok);
  auto packet = previous[0];
  const auto unchanged = inputs[1].pads[0].size();
  auto bad = packet; bad[0] = NP_MSG_SLIPPI_PAD;
  CHECK(!inputs[1].receive(bad.data(),bad.size(),0).ok);
  bad = packet; bad[4] = 0; CHECK(!inputs[1].receive(bad.data(),bad.size(),0).ok);
  bad = packet; bad[6] = 3; CHECK(!inputs[1].receive(bad.data(),bad.size(),0).ok);
  CHECK(!inputs[1].receive(packet.data(),packet.size()-1,0).ok);
  CHECK(!inputs[1].receive(packet.data(),packet.size(),3).ok); // a different connection cannot send the pair
  CHECK(inputs[1].pads[0].size() == unchanged);
}
int main() {
  simulation({{0,0,2,3}},{0,2,3},4); // pair + two remote humans: three machines, four ports
  simulation({{0,0,2,2}},{0,2},4);   // pair + pair: two machines
  simulation({{0,0,2,3}},{0,2},3);   // pair + solo, CPU on the last port
  CHECK(mu_local_partner_capable(MU_BUILD_CAPS2_FRIENDS_LOBBY | MU_BUILD_CAPS2_LOCAL_PARTNER));
  CHECK(!mu_local_partner_capable(MU_BUILD_CAPS2_FRIENDS_LOBBY)); // old build refused
  // Slippi 3.7.0's 27-byte TXB, plus the local partner's nine-byte pad.
  uint8_t payload[36] = {};
  CHECK(slippi::online::command_payload_valid(slippi::online::CMD_ONLINE_INPUTS,payload,27));
  CHECK(slippi::online::command_payload_valid(slippi::online::CMD_ONLINE_INPUTS,payload,36));
  for (uint32_t old : {25u,33u,35u}) CHECK(!slippi::online::command_payload_valid(slippi::online::CMD_ONLINE_INPUTS,payload,old));
  for (int machines : {1,2}) {
    const auto ticket = yougame::friends::doubles_queue_ticket("doubles-lobby-1:mu","abcdefghjkmn","generation",machines);
    CHECK(ticket["local_partner"] == yougame::friends::kLocalPartnerCapability && ticket["queue"] == "casual");
    CHECK(ticket["lobby"]["maxLocalPlayers"] == 1 && !ticket.count("local_players"));
    CHECK(ticket.count("party") == size_t(machines == 2));
  }
  std::puts("ok");
}
