#include "practice_ghosts.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
template<size_t N> static void zero(const uint8_t (&odds)[N]) { for (auto n : odds) CHECK(n == 0); }
int main() {
  using namespace practice_ghosts;
  CHECK(std::size(kGhosts) == 5);
  const int kinds[] = {22, 18, 15, 1, 14};
  const char* characters[] = {"Falco", "Marth", "Jigglypuff", "Fox", "Yoshi"};
  for (size_t i = 0; i < std::size(kGhosts); ++i) {
    const auto& g = kGhosts[i];
    CHECK(g.name == "Pro " + std::to_string(i + 1));
    CHECK(std::string(g.character) == characters[i]);
    CHECK(g.kind == kinds[i]);
    CHECK(g.games == 0 && g.sampled == 0 && !kReleased[i]);
    zero(g.move); zero(g.attack); zero(g.defend); zero(g.oos); zero(g.di);
    zero(g.tech); zero(g.getup); zero(g.ledge); zero(g.after);
    CHECK(g.l_cancel == 0 && g.aggression == 0);
  }
  std::printf("ghost placeholders: %d/%d checks passed\n", checks, checks);
}
