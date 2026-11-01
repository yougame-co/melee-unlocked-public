// Golden checksums for every baked tier, including the Challenger motion layers.
// The Python source test separately re-rasterizes the actual TSX paths.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mu_rank_badges.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static uint32_t checksum(const unsigned char* bytes, unsigned size) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
int main() {
    constexpr uint32_t golden[] = {
        0x7173d645,0x7173d645,0x3a54831d,0xda3b8c3d,0x7173d645,0x1ae90115,
        0x5d506145,0x6eda4ee5,0xd3fdf3f4,0x8028f49d,0xa473d4d4,0x4394c822,
        0xd8e64a76,0xab49f39e,0xe55847f6,0x165e6761
    };
    CHECK(MU_RANK_BADGE_SIZE == 48 && MU_RANK_BADGE_SIZE % 4 == 0);
    CHECK(MU_RANK_BADGE_TEXTURES == 16);
    CHECK(sizeof(mu_badge_bronze_placed) == 48*48*4);
    CHECK(sizeof(mu_badge_challenger) == sizeof(mu_badge_bronze_placed));
    CHECK(mu_rank_tier_rgb[1] == 0xc98a5b && mu_rank_tier_rgb[6] == 0xffc857);
    for (unsigned i = 0; i < MU_RANK_BADGE_TEXTURES; ++i) {
        auto* data = mu_badge_textures[i];
        CHECK(reinterpret_cast<uintptr_t>(data) % 32 == 0);
        CHECK(checksum(data, 48*48*4) == golden[i]);
        // Independently decode GX's AR/GB 4x4 blocks. Transparent corners and
        // partially covered edges ensure alpha, tiling and antialiasing survive.
        int transparent = 0, opaque = 0, edge = 0;
        for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 48; ++x) {
            unsigned block = ((y/4)*12+x/4)*64;
            unsigned pixel = (y%4)*4+x%4;
            unsigned alpha = data[block+pixel*2];
            transparent += alpha == 0; opaque += alpha == 255;
            edge += alpha > 0 && alpha < 255;
        }
        CHECK(transparent > 100 && opaque > 0 && edge > 0);
        CHECK(data[0] == 0); // Top-left texel alpha, not a stray channel.
    }
    std::cout << "rank badges: " << checks << " checks passed\n";
}
