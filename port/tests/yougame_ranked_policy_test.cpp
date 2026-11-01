// Selection bytes, stage mapping and complete digest agreement.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yougame_ranked_rules.h"
#include <cstdlib>
#include <iostream>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
int main() {
  using namespace yougame;
  const auto policy = ranked_policy::slippi_v1();
  const auto bytes = ranked_policy::encode(policy);
  const std::vector<uint8_t> expected = {
    1,1, 0,2,0,28,0,8,0,32,0,31, 0,2,0,28,0,8,0,31,0,32,0,3,
    0,1,0, 1,2,1, 0,1, 0,1,0,1, 1,1,1, 25,25,30,30,10,30,30,45,45,3,15};
  CHECK(bytes == expected && bytes.size() == 50);
  MuRankedPolicy decoded{}; CHECK(mu_yg_policy_decode(&decoded, bytes.data(), bytes.size()));
  CHECK(ranked_policy::encode(decoded) == expected);
  CHECK(!mu_yg_policy_decode(&decoded, bytes.data(), 49) && decoded.available == 0);
  auto future = bytes; future[0] = 2; CHECK(!mu_yg_policy_decode(&decoded, future.data(), future.size()));
  const auto base = ranked_rules::vanilla();
  CHECK(ranked_rules::selection_error(base) == nullptr && base.digest.size() == 64);
  ranked_rules::Rules remix;
  CHECK(ranked_rules::parse(R"({"version":1,"stages":[32,28,8,3,31,2],"characters":"all"})", false, remix));
  CHECK(remix.digest == base.digest && !ranked_rules::selection_error(remix));
  for (const auto& stages : {std::vector<uint16_t>{2,8,28,31,32}, {2,3,8,28,31,32,85}, {2,3,8,28,31,65535}}) {
    auto unsupported = base; unsupported.stages = stages;
    CHECK(std::string(ranked_rules::selection_error(unsupported)) == ranked_rules::kUnmapped);
  }
  // Every byte, including order, timers, DSR and color policy, affects peer agreement.
  for (size_t i = 0; i < bytes.size(); ++i) {
    auto changed = bytes; changed[i] ^= 1;
    auto other = base;
    if (i == 0) other.policy.version ^= 1;
    else { CHECK(mu_yg_policy_decode(&other.policy, changed.data(), changed.size())); }
    ranked_rules::digest(other);
    CHECK(other.digest != base.digest);
    CHECK(ranked_rules::peer_verdict(base.digest, true, other.digest, false) == -1);
  }
  // Both seats must supply a full policy digest even for base Melee.
  CHECK(ranked_rules::peer_verdict(base.digest, false, "", false) == 0);
  CHECK(ranked_rules::peer_verdict(base.digest, false, "", true) == -1);
  CHECK(ranked_rules::peer_verdict(base.digest, true, "", false) == -1);
  CHECK(ranked_rules::peer_verdict(base.digest, true, base.digest, false) == 1);
  std::cout << "ranked selection policy: " << checks << " checks passed\n";
}
